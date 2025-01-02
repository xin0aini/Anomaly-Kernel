/*
 *  linux/drivers/devfreq/governor_echelon.c
 *
 *  Copyright (C) 2025 The_Anomalist
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/devfreq.h>
#include <linux/math64.h>
#include <linux/ktime.h>
#include <linux/thermal.h>  /* For temperature-based scaling */
#include <linux/sched/clock.h>
#include "governor.h"

/* Echelon: Gaming-optimized DevFreq Governor for Adreno 650 */
#define ECHELON_UPTHRESHOLD       (85)  /* 85% load for scaling up */
#define ECHELON_DOWNTHRESHOLD     (30)  /* 30% load for scaling down */
#define ECHELON_DOWNSCALE_FACTOR  (50)  /* Frequency reduction factor on idle */
#define ECHELON_SCALE_TIMEOUT     (100) /* Timeout for scaling in ms */
#define ECHELON_MAX_SCALE_STEP    (2)   /* Maximum step for scaling frequency */
#define THERMAL_ZONE_NAME         "thermal_zone0" /* Assuming thermal zone0 */

/* Data structure to hold Echelon governor settings */
struct devfreq_echelon_data {
    unsigned int upthreshold;
    unsigned int downthreshold;
    unsigned int downscale_factor;
    unsigned int max_scale_step;
    ktime_t last_update_time; /* Time of last frequency update */
};

/* Frequency scaling function */
static int devfreq_echelon_func(struct devfreq *df, unsigned long *freq)
{
    int err;
    struct devfreq_dev_status *stat;
    struct devfreq_echelon_data *data = df->data;
    unsigned long max = (df->max_freq) ? df->max_freq : UINT_MAX;
    unsigned long min = (df->min_freq) ? df->min_freq : 0;

    /* Declare variables at the start to comply with C90 */
    unsigned int upthreshold = ECHELON_UPTHRESHOLD;
    unsigned int downthreshold = ECHELON_DOWNTHRESHOLD;
    unsigned int downscale_factor = ECHELON_DOWNSCALE_FACTOR;
    unsigned int max_scale_step = ECHELON_MAX_SCALE_STEP;
    unsigned long predicted_frequency = *freq;

    static bool scaling_up = false;
    static bool scaling_down = false;
    
    struct thermal_zone_device *tz = NULL; /* Declare thermal zone device here */

    err = devfreq_update_stats(df);
    if (err)
        return err;

    stat = &df->last_status;

    if (data) {
        if (data->upthreshold)
            upthreshold = data->upthreshold;
        if (data->downthreshold)
            downthreshold = data->downthreshold;
        if (data->downscale_factor)
            downscale_factor = data->downscale_factor;
        if (data->max_scale_step)
            max_scale_step = data->max_scale_step;
    }

    /* Prevent overflow */
    if (stat->busy_time >= (1 << 24) || stat->total_time >= (1 << 24)) {
        stat->busy_time >>= 7;
        stat->total_time >>= 7;
    }

    /* Handle GPU load scaling */
    if (stat->total_time == 0) {
        *freq = max;
        return 0;
    }

    /* Proactive Scaling: Preemptively adjust frequency based on increasing load */
    if (stat->busy_time * 100 > stat->total_time * upthreshold) {
        predicted_frequency = max; // Scale up if load is high
    } else if (stat->busy_time * 100 < stat->total_time * downthreshold) {
        predicted_frequency = min + downscale_factor; // Scale down for idle
    }

    if (predicted_frequency != *freq) {
        *freq = predicted_frequency;
    }

    /* Apply hysteresis to prevent oscillations */
    if (scaling_up && *freq != max) {
        scaling_up = false;
    }
    if (scaling_down && *freq != min) {
        scaling_down = false;
    }

    if (stat->busy_time * 100 > stat->total_time * upthreshold && !scaling_up) {
        *freq = max;
        scaling_up = true;
    } else if (stat->busy_time * 100 < stat->total_time * downthreshold && !scaling_down) {
        *freq = min + downscale_factor;
        scaling_down = true;
    }

    /* Keep the frequency within min/max bounds */
    if (*freq < min)
        *freq = min;
    if (*freq > max)
        *freq = max;

    /* Apply step-based scaling for smooth frequency changes */
    if (stat->current_frequency > *freq + max_scale_step) {
        *freq = stat->current_frequency - max_scale_step;
    } else if (stat->current_frequency < *freq - max_scale_step) {
        *freq = stat->current_frequency + max_scale_step;
    }

    /* Thermal management: Integrate cooling-based frequency scaling */
    tz = thermal_zone_get_zone_by_name(THERMAL_ZONE_NAME);
    if (!IS_ERR(tz)) {
        int temp;
        err = thermal_zone_get_temp(tz, &temp);
        if (err == 0) {
            /* Scale down frequency if temperature exceeds 80°C */
            if (temp > 80000) { /* 80°C */
                *freq = min;
            }
        }
    }

    /* Logging and diagnostic support (optional, can be enabled for debugging) */
    pr_debug("Echelon Governor: Load: %lu%%, Current Freq: %lu, Predicted Freq: %lu\n", 
             stat->busy_time * 100 / stat->total_time, stat->current_frequency, *freq);

    return 0;
}

/* Event handler for Echelon governor */
static int devfreq_echelon_handler(struct devfreq *devfreq,
                                   unsigned int event, void *data)
{
    switch (event) {
    case DEVFREQ_GOV_START:
        devfreq_monitor_start(devfreq);
        break;

    case DEVFREQ_GOV_STOP:
        devfreq_monitor_stop(devfreq);
        break;

    case DEVFREQ_GOV_INTERVAL:
        devfreq_interval_update(devfreq, (unsigned int *)data);
        break;

    case DEVFREQ_GOV_SUSPEND:
        devfreq_monitor_suspend(devfreq);
        break;

    case DEVFREQ_GOV_RESUME:
        devfreq_monitor_resume(devfreq);
        break;

    default:
        break;
    }

    return 0;
}

static struct devfreq_governor devfreq_echelon = {
    .name = "Echelon",
    .get_target_freq = devfreq_echelon_func,
    .event_handler = devfreq_echelon_handler,
};

static int __init devfreq_echelon_init(void)
{
    return devfreq_add_governor(&devfreq_echelon);
}
subsys_initcall(devfreq_echelon_init);

static void __exit devfreq_echelon_exit(void)
{
    int ret;

    ret = devfreq_remove_governor(&devfreq_echelon);
    if (ret)
        pr_err("%s: failed remove governor %d\n", __func__, ret);

    return;
}
module_exit(devfreq_echelon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("The_Anomalist");
MODULE_DESCRIPTION("Echelon: A high-performance devfreq governor for Adreno 650 GPU, optimized for gaming.");

