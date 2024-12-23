// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/msm_adreno_devfreq.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

#include "devfreq_trace.h"
#include "governor.h"

#define MIN_BUSY                1000
#define LONG_FLOOR              50000
#define HIST                    5
#define TARGET                  80
#define CAP                     75
#define WAIT_THRESHOLD          10
/* AB vote is in multiple of BW_STEP Mega bytes */
#define BW_STEP                 160

/* Dynamic adjustments for sampling intervals */
#define HIGH_GPU_LOAD_THRESHOLD 90
#define LOW_GPU_LOAD_THRESHOLD  20
#define HIGH_SAMPLING_MS        10
#define LOW_SAMPLING_MS         50

static void _update_cutoff(struct devfreq_msm_adreno_tz_data *priv,
                           unsigned int norm_max)
{
    int i;

    priv->bus.max = norm_max;
    for (i = 0; i < priv->bus.num; i++) {
        priv->bus.up[i] = priv->bus.p_up[i] * norm_max / 100;
        priv->bus.down[i] = priv->bus.p_down[i] * norm_max / 100;
    }
}

static inline int devfreq_get_freq_level(struct devfreq *devfreq,
                                         unsigned long freq)
{
    int lev;

    for (lev = 0; lev < devfreq->profile->max_state; lev++)
        if (freq == devfreq->profile->freq_table[lev])
            return lev;

    return -EINVAL;
}

static ssize_t cur_ab_show(struct device *dev,
                           struct device_attribute *attr,
                           char *buf)
{
    struct devfreq *df = to_devfreq(dev);
    struct msm_busmon_extended_profile *bus_profile = container_of(
        (df->profile),
        struct msm_busmon_extended_profile,
        profile);

    return scnprintf(buf, PAGE_SIZE, "%llu\n", bus_profile->ab_mbytes);
}

static ssize_t sampling_interval_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf)
{
    struct devfreq *df = to_devfreq(dev);
    struct msm_busmon_extended_profile *bus_profile = container_of(
        (df->profile),
        struct msm_busmon_extended_profile,
        profile);

    return scnprintf(buf, PAGE_SIZE, "%d\n", bus_profile->sampling_ms);
}

static ssize_t sampling_interval_store(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count)
{
    struct devfreq *df = to_devfreq(dev);
    struct msm_busmon_extended_profile *bus_profile = container_of(
        (df->profile),
        struct msm_busmon_extended_profile,
        profile);
    u32 value;
    int ret;

    ret = kstrtou32(buf, 0, &value);
    if (ret)
        return ret;

    bus_profile->sampling_ms = value;

    return count;
}

static DEVICE_ATTR_RW(sampling_interval);
static DEVICE_ATTR_RO(cur_ab);

static const struct device_attribute *gpubw_attr_list[] = {
    &dev_attr_sampling_interval,
    &dev_attr_cur_ab,
    NULL
};

static int devfreq_gpubw_get_target(struct devfreq *df,
                                    unsigned long *freq)
{
    struct devfreq_msm_adreno_tz_data *priv = df->data;
    struct msm_busmon_extended_profile *bus_profile = container_of(
        (df->profile),
        struct msm_busmon_extended_profile,
        profile);
    struct devfreq_dev_status *stats = &df->last_status;
    struct xstats b;
    int result;
    int level = 0;
    int act_level;
    int norm_max_cycles;
    int norm_cycles;
    int wait_active_percent;
    int gpu_percent;
    static int norm_ab_max = 300;
    int norm_ab;
    unsigned long ab_mbytes = 0;

    if (!priv)
        return 0;

    stats->private_data = &b;
    result = devfreq_update_stats(df);
    *freq = stats->current_frequency;

    priv->bus.total_time += stats->total_time;
    priv->bus.gpu_time += stats->busy_time;
    priv->bus.ram_time += b.ram_time;
    priv->bus.ram_wait += b.ram_wait;

    level = devfreq_get_freq_level(df, stats->current_frequency);

    if (priv->bus.total_time < bus_profile->sampling_ms)
        return result;

    norm_max_cycles = priv->bus.ram_time / priv->bus.total_time;
    norm_cycles = (priv->bus.ram_time + priv->bus.ram_wait) / priv->bus.total_time;

    wait_active_percent = priv->bus.ram_wait == 0 ? 0 :
                          priv->bus.ram_time == 0 ? 100 :
                          (100 * priv->bus.ram_wait) / priv->bus.ram_time;

    gpu_percent = (100 * priv->bus.gpu_time) / priv->bus.total_time;

    /* Dynamic sampling interval adjustment */
    if (gpu_percent > HIGH_GPU_LOAD_THRESHOLD)
        bus_profile->sampling_ms = HIGH_SAMPLING_MS;
    else if (gpu_percent < LOW_GPU_LOAD_THRESHOLD)
        bus_profile->sampling_ms = LOW_SAMPLING_MS;

    if (norm_max_cycles > priv->bus.max && priv->bus.floating) {
        _update_cutoff(priv, norm_max_cycles);
        bus_profile->flag = DEVFREQ_FLAG_FAST_HINT;
    } else {
        norm_cycles = (100 * norm_cycles) / TARGET;
        act_level = priv->bus.index[level] + b.mod;
        act_level = clamp(act_level, 0, (int)(priv->bus.num - 1));  // Fix applied here

        if ((norm_cycles > priv->bus.up[act_level] ||
             wait_active_percent > WAIT_THRESHOLD) &&
            gpu_percent > CAP) {
            bus_profile->flag = DEVFREQ_FLAG_FAST_HINT;
        } else if (norm_cycles < priv->bus.down[act_level] && level) {
            bus_profile->flag = DEVFREQ_FLAG_SLOW_HINT;
        }
    }

    if (priv->bus.width) {
        norm_ab = priv->bus.ram_time / priv->bus.total_time;
        ab_mbytes = (norm_ab * priv->bus.width * 1000000ULL) >> 20;
        bus_profile->ab_mbytes = roundup(ab_mbytes, BW_STEP);
    } else if (bus_profile->flag) {
        norm_ab = priv->bus.ram_time / priv->bus.total_time;
        norm_ab_max = max(norm_ab, norm_ab_max);
        bus_profile->percent_ab = (100 * norm_ab) / norm_ab_max;
    }

    priv->bus.total_time = 0;
    priv->bus.gpu_time = 0;
    priv->bus.ram_time = 0;
    priv->bus.ram_wait = 0;

    return result;
}

static int gpubw_start(struct devfreq *devfreq)
{
    struct devfreq_msm_adreno_tz_data *priv;
    struct msm_busmon_extended_profile *bus_profile = container_of(
        (devfreq->profile),
        struct msm_busmon_extended_profile,
        profile);
    unsigned int t1, t2 = 2 * HIST;
    int i, bus_size;

    devfreq->data = bus_profile->private_data;
    priv = devfreq->data;

    bus_size = sizeof(u32) * priv->bus.num;
    priv->bus.up = kzalloc(bus_size, GFP_KERNEL);
    priv->bus.down = kzalloc(bus_size, GFP_KERNEL);
    priv->bus.p_up = kzalloc(bus_size, GFP_KERNEL);
    priv->bus.p_down = kzalloc(bus_size, GFP_KERNEL);

    if (!priv->bus.up || !priv->bus.down || !priv->bus.p_up || !priv->bus.p_down)
        return -ENOMEM;

    for (i = 0; i < priv->bus.num; i++) {
        t1 = (100 * priv->bus.ib[i]) / priv->bus.ib[priv->bus.num - 1];
        priv->bus.p_up[i] = t1 - HIST;
        priv->bus.p_down[i] = t2 - 2 * HIST;
        t2 = t1;
    }

    priv->bus.p_down[0] = 0;
    priv->bus.p_down[1] = max(priv->bus.p_down[1], 2 * HIST);
    if (priv->bus.num >= 1)
        priv->bus.p_up[priv->bus.num - 1] = 100;
    _update_cutoff(priv, priv->bus.max);

    bus_profile->sampling_ms = LONG_FLOOR;

    for (i = 0; gpubw_attr_list[i] != NULL; i++)
        device_create_file(&devfreq->dev, gpubw_attr_list[i]);

    return 0;
}

static int gpubw_stop(struct devfreq *devfreq)
{
    struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
    int i;

    for (i = 0; gpubw_attr_list[i] != NULL; i++)
        device_remove_file(&devfreq->dev, gpubw_attr_list[i]);

    if (priv) {
        kfree(priv->bus.up);
        kfree(priv->bus.down);
        kfree(priv->bus.p_up);
        kfree(priv->bus.p_down);
    }
    devfreq->data = NULL;
    return 0;
}

static int devfreq_gpubw_event_handler(struct devfreq *devfreq,
                                       unsigned int event, void *data)
{
    int result = 0;
    struct device_node *node = devfreq->dev.parent->of_node;

    if (!of_device_is_compatible(node, "qcom,kgsl-busmon"))
        return -EINVAL;

    mutex_lock(&devfreq->lock);
    switch (event) {
    case DEVFREQ_GOV_START:
        result = gpubw_start(devfreq);
        break;
    case DEVFREQ_GOV_STOP:
        result = gpubw_stop(devfreq);
        break;
    case DEVFREQ_GOV_RESUME:
        break;
    case DEVFREQ_GOV_SUSPEND:
        {
            struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
            if (priv) {
                priv->bus.total_time = 0;
                priv->bus.gpu_time = 0;
                priv->bus.ram_time = 0;
            }
        }
        break;
    default:
        break;
    }
    mutex_unlock(&devfreq->lock);
    return result;
}

static struct devfreq_governor devfreq_gpubw = {
    .name = "gpubw_mon",
    .get_target_freq = devfreq_gpubw_get_target,
    .event_handler = devfreq_gpubw_event_handler,
};

static int __init devfreq_gpubw_init(void)
{
    return devfreq_add_governor(&devfreq_gpubw);
}
subsys_initcall(devfreq_gpubw_init);

static void __exit devfreq_gpubw_exit(void)
{
    int ret = devfreq_remove_governor(&devfreq_gpubw);
    if (ret)
        pr_err("%s: failed to remove governor %d\n", __func__, ret);
}
module_exit(devfreq_gpubw_exit);

MODULE_DESCRIPTION("GPU bus bandwidth voting driver. Uses VBIF counters");
MODULE_LICENSE("GPL v2");

