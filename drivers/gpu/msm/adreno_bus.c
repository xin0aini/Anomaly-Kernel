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
#include <linux/device.h> // For device_kobj

/* Define bus frequency control structure */
struct adreno_bus_freq_status {
    unsigned int bus_min;
    unsigned int bus_max;
};

static struct adreno_bus_freq_status adreno_bus_freq_status = {
    .bus_min = 200, // Default min bus frequency (MHz)
    .bus_max = 900, // Default max bus frequency (MHz)
};

// Define bus levels (Low, Medium, High)
enum adreno_bus_level {
    ADRENO_BUS_LOW,
    ADRENO_BUS_MEDIUM,
    ADRENO_BUS_HIGH,
};

static void adreno_set_bus_level(enum adreno_bus_level level)
{
    switch (level) {
    case ADRENO_BUS_LOW:
        adreno_bus_freq_status.bus_min = 200;
        adreno_bus_freq_status.bus_max = 500;
        pr_info("Set adreno bus frequencies to Low: %u MHz - %u MHz\n",
                adreno_bus_freq_status.bus_min, adreno_bus_freq_status.bus_max);
        break;
    case ADRENO_BUS_MEDIUM:
        adreno_bus_freq_status.bus_min = 500;
        adreno_bus_freq_status.bus_max = 700;
        pr_info("Set adreno bus frequencies to Medium: %u MHz - %u MHz\n",
                adreno_bus_freq_status.bus_min, adreno_bus_freq_status.bus_max);
        break;
    case ADRENO_BUS_HIGH:
        adreno_bus_freq_status.bus_min = 700;
        adreno_bus_freq_status.bus_max = 900;
        pr_info("Set adreno bus frequencies to High: %u MHz - %u MHz\n",
                adreno_bus_freq_status.bus_min, adreno_bus_freq_status.bus_max);
        break;
    default:
        pr_err("Invalid adreno bus level\n");
        break;
    }
}

/* SysFS attribute for adreno_bus */
static ssize_t adreno_bus_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%u\n", adreno_bus_freq_status.bus_min);
}

static ssize_t adreno_bus_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    unsigned int val;
    if (kstrtouint(buf, 10, &val)) {
        return -EINVAL;
    }

    switch (val) {
    case 0:
        adreno_set_bus_level(ADRENO_BUS_LOW);
        break;
    case 1:
        adreno_set_bus_level(ADRENO_BUS_MEDIUM);
        break;
    case 2:
        adreno_set_bus_level(ADRENO_BUS_HIGH);
        break;
    default:
        pr_err("Invalid adreno bus level. Use 0 (Low), 1 (Medium), or 2 (High)\n");
        return -EINVAL;
    }

    return count;
}

static struct kobj_attribute adreno_bus_attr = __ATTR(adreno_bus, 0664, adreno_bus_show, adreno_bus_store);

/* Kobject for sysfs */
static struct kobject *adreno_kobj;

/* Initialize the adreno bus frequency controls */
int adreno_bus_init(void)
{
    int ret;

    /* Create kobject under the GPU class or kernel kobject */
    pr_debug("Creating adreno_bus kobject...\n");
    adreno_kobj = kobject_create_and_add("adreno_bus", kernel_kobj); // Or adjust to use GPU class
    if (!adreno_kobj) {
        pr_err("Failed to create adreno_kobj\n");
        return -ENOMEM;
    }

    pr_debug("Creating sysfs files for bus_min, bus_max, and adreno_bus...\n");

    ret = sysfs_create_file(adreno_kobj, &adreno_bus_attr.attr);
    if (ret) {
        pr_err("Failed to create adreno_bus attribute\n");
        kobject_put(adreno_kobj);
        return ret;
    }

    pr_info("Adreno bus frequency control initialized.\n");
    return 0;
}

/* Cleanup the adreno bus frequency controls */
void adreno_bus_exit(void)
{
    pr_debug("Cleaning up adreno_bus sysfs files...\n");

    if (adreno_kobj) {
        sysfs_remove_file(adreno_kobj, &adreno_bus_attr.attr);
        kobject_put(adreno_kobj);
        adreno_kobj = NULL; // Clear pointer after cleanup
    }

    pr_info("Adreno bus frequency control cleaned up.\n");
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Adreno Bus Frequency Control with Levels");
MODULE_AUTHOR("The_Anomalist");

