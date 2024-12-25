// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2002,2007-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_fdt.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

/* Define bus frequency control structure */
struct adreno_bus_freq_status {
    unsigned int bus_min;
    unsigned int bus_max;
};

static struct adreno_bus_freq_status adreno_bus_freq_status = {
    .bus_min = 200, // Default min bus frequency (MHz)
    .bus_max = 900, // Default max bus frequency (MHz)
};

/* SysFS attribute for bus_min */
static ssize_t bus_min_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%u\n", adreno_bus_freq_status.bus_min);
}

static ssize_t bus_min_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    unsigned int val;
    if (kstrtouint(buf, 10, &val) || val < 200 || val > 900)
        return -EINVAL;

    adreno_bus_freq_status.bus_min = val;
    pr_info("Set bus min frequency to %u MHz\n", adreno_bus_freq_status.bus_min);
    return count;
}

static struct kobj_attribute bus_min_attr = __ATTR(bus_min, 0664, bus_min_show, bus_min_store);

/* SysFS attribute for bus_max */
static ssize_t bus_max_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%u\n", adreno_bus_freq_status.bus_max);
}

static ssize_t bus_max_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    unsigned int val;
    if (kstrtouint(buf, 10, &val) || val < 200 || val > 900)
        return -EINVAL;

    adreno_bus_freq_status.bus_max = val;
    pr_info("Set bus max frequency to %u MHz\n", adreno_bus_freq_status.bus_max);
    return count;
}

static struct kobj_attribute bus_max_attr = __ATTR(bus_max, 0664, bus_max_show, bus_max_store);

/* Kobject for sysfs */
static struct kobject *adreno_kobj;

/* Initialize the bus frequency controls */
int adreno_bus_init(void)
{
    int ret;

    adreno_kobj = kobject_create_and_add("adreno_bus", kernel_kobj);
    if (!adreno_kobj) {
        pr_err("Failed to create adreno_kobj\n");
        return -ENOMEM;
    }

    ret = sysfs_create_file(adreno_kobj, &bus_min_attr.attr);
    if (ret) {
        pr_err("Failed to create bus_min attribute\n");
        goto fail_min;
    }

    ret = sysfs_create_file(adreno_kobj, &bus_max_attr.attr);
    if (ret) {
        pr_err("Failed to create bus_max attribute\n");
        goto fail_max;
    }

    pr_info("Adreno bus frequency control initialized.\n");
    return 0;

fail_max:
    sysfs_remove_file(adreno_kobj, &bus_min_attr.attr);
fail_min:
    kobject_put(adreno_kobj);
    return ret;
}

/* Cleanup the bus frequency controls */
void adreno_bus_exit(void)
{
    if (adreno_kobj) {
        sysfs_remove_file(adreno_kobj, &bus_min_attr.attr);
        sysfs_remove_file(adreno_kobj, &bus_max_attr.attr);
        kobject_put(adreno_kobj);
        adreno_kobj = NULL; // Clear pointer after cleanup
    }
    pr_info("Adreno bus frequency control cleaned up.\n");
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Adreno Bus Frequency Control");
MODULE_AUTHOR("The_Anomalist");

