// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Surface Integration Driver.
 * MFD driver to provide device/model dependent functionality.
 */

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>

#include "surface_sam_sid_power.h"
#include "surface_sam_sid_vhf.h"


static const struct ssam_battery_properties ssam_battery_props_bat1 = {
	.registry = SSAM_EVENT_REGISTRY_SAM,
	.num      = 0,
	.channel  = 1,
	.instance = 1,
};

static const struct ssam_battery_properties ssam_battery_props_bat2_sb3 = {
	.registry = SSAM_EVENT_REGISTRY_KIP,
	.num      = 1,
	.channel  = 2,
	.instance = 1,
};


static const struct ssam_hid_properties ssam_hid_props_keyboard = {
	.registry = SSAM_EVENT_REGISTRY_REG,
	.instance = 1,
};

static const struct ssam_hid_properties ssam_hid_props_touchpad = {
	.registry = SSAM_EVENT_REGISTRY_REG,
	.instance = 3,
};

static const struct ssam_hid_properties ssam_hid_props_iid5 = {
	.registry = SSAM_EVENT_REGISTRY_REG,
	.instance = 5,
};

static const struct ssam_hid_properties ssam_hid_props_iid6 = {
	.registry = SSAM_EVENT_REGISTRY_REG,
	.instance = 6,
};


static const struct mfd_cell sid_devs_sp4[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ },
};

static const struct mfd_cell sid_devs_sp6[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ },
};

static const struct mfd_cell sid_devs_sp7[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ .name = "surface_sam_sid_ac",       .id = -1 },
	{
		.name = "surface_sam_sid_battery",
		.id = -1,
		.platform_data = (void *)&ssam_battery_props_bat1,
		.pdata_size = sizeof(struct ssam_battery_properties),
	},
	{ },
};

static const struct mfd_cell sid_devs_sb1[] = {
	{ .name = "surface_sam_sid_gpelid", .id = -1 },
	{ },
};

static const struct mfd_cell sid_devs_sb2[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ },
};

static const struct mfd_cell sid_devs_sb3[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ .name = "surface_sam_sid_ac",       .id = -1 },
	{
		.name = "surface_sam_sid_battery",
		.id = 1,
		.platform_data = (void *)&ssam_battery_props_bat1,
		.pdata_size = sizeof(struct ssam_battery_properties),
	},
	{
		.name = "surface_sam_sid_battery",
		.id = 2,
		.platform_data = (void *)&ssam_battery_props_bat2_sb3,
		.pdata_size = sizeof(struct ssam_battery_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 1,
		.platform_data = (void *)&ssam_hid_props_keyboard,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 3,
		.platform_data = (void *)&ssam_hid_props_touchpad,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 5,
		.platform_data = (void *)&ssam_hid_props_iid5,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 6,
		.platform_data = (void *)&ssam_hid_props_iid6,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{ },
};

static const struct mfd_cell sid_devs_sl1[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ },
};

static const struct mfd_cell sid_devs_sl2[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ },
};

static const struct mfd_cell sid_devs_sl3_13[] = {
	{ .name = "surface_sam_sid_gpelid",   .id = -1 },
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ .name = "surface_sam_sid_ac",       .id = -1 },
	{
		.name = "surface_sam_sid_battery",
		.id = -1,
		.platform_data = (void *)&ssam_battery_props_bat1,
		.pdata_size = sizeof(struct ssam_battery_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 1,
		.platform_data = (void *)&ssam_hid_props_keyboard,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 3,
		.platform_data = (void *)&ssam_hid_props_touchpad,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 5,
		.platform_data = (void *)&ssam_hid_props_iid5,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{ },
};

static const struct mfd_cell sid_devs_sl3_15[] = {
	{ .name = "surface_sam_sid_perfmode", .id = -1 },
	{ .name = "surface_sam_sid_ac",       .id = -1 },
	{
		.name = "surface_sam_sid_battery",
		.id = -1,
		.platform_data = (void *)&ssam_battery_props_bat1,
		.pdata_size = sizeof(struct ssam_battery_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 1,
		.platform_data = (void *)&ssam_hid_props_keyboard,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 3,
		.platform_data = (void *)&ssam_hid_props_touchpad,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{
		.name = "surface_sam_sid_vhf",
		.id = 5,
		.platform_data = (void *)&ssam_hid_props_iid5,
		.pdata_size = sizeof(struct ssam_hid_properties),
	},
	{ },
};

static const struct acpi_device_id surface_sam_sid_match[] = {
	/* Surface Pro 4, 5, and 6 */
	{ "MSHW0081", (unsigned long)sid_devs_sp4 },

	/* Surface Pro 6 (OMBR >= 0x10) */
	{ "MSHW0111", (unsigned long)sid_devs_sp6 },

	/* Surface Pro 7 */
	{ "MSHW0116", (unsigned long)sid_devs_sp7 },

	/* Surface Book 1 */
	{ "MSHW0080", (unsigned long)sid_devs_sb1 },

	/* Surface Book 2 */
	{ "MSHW0107", (unsigned long)sid_devs_sb2 },

	/* Surface Book 3 */
	{ "MSHW0117", (unsigned long)sid_devs_sb3 },

	/* Surface Laptop 1 */
	{ "MSHW0086", (unsigned long)sid_devs_sl1 },

	/* Surface Laptop 2 */
	{ "MSHW0112", (unsigned long)sid_devs_sl2 },

	/* Surface Laptop 3 (13") */
	{ "MSHW0114", (unsigned long)sid_devs_sl3_13 },

	/* Surface Laptop 3 (15") */
	{ "MSHW0110", (unsigned long)sid_devs_sl3_15 },

	{ },
};
MODULE_DEVICE_TABLE(acpi, surface_sam_sid_match);


static int surface_sam_sid_probe(struct platform_device *pdev)
{
	const struct acpi_device_id *match;
	const struct mfd_cell *cells, *p;

	match = acpi_match_device(surface_sam_sid_match, &pdev->dev);
	if (!match)
		return -ENODEV;

	cells = (struct mfd_cell *)match->driver_data;
	if (!cells)
		return -ENODEV;

	for (p = cells; p->name; ++p) {
		/* just count */
	}

	if (p == cells)
		return -ENODEV;

	return mfd_add_devices(&pdev->dev, 0, cells, p - cells, NULL, 0, NULL);
}

static int surface_sam_sid_remove(struct platform_device *pdev)
{
	mfd_remove_devices(&pdev->dev);
	return 0;
}

static struct platform_driver surface_sam_sid = {
	.probe = surface_sam_sid_probe,
	.remove = surface_sam_sid_remove,
	.driver = {
		.name = "surface_sam_sid",
		.acpi_match_table = surface_sam_sid_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(surface_sam_sid);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Surface Integration Driver for 5th Generation Surface Devices");
MODULE_LICENSE("GPL");
