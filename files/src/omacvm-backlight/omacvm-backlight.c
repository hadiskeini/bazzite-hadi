// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Mac's display brightness in the VM, as the backlight "omacvm" (0-100). The VM's display has no
 * backlight of its own; with this one the desktop's own brightness slider, keys and popup work. The user's
 * agent (omacvm-mac-levels) passes changes to the Mac through OmacVM Bridge and writes the Mac's changes here.
 * Nothing here talks to the Mac: a write only stores the value, and the backlight core tells user space.
 */
#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static struct platform_device *omb_pdev;
static struct backlight_device *omb_bd;

static int omb_update_status(struct backlight_device *bd)
{
	return 0;
}

static const struct backlight_ops omb_ops = {
	.update_status = omb_update_status,
};

static int __init omb_init(void)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_FIRMWARE,
		.max_brightness = 100,
		.brightness = 100,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};

	omb_pdev = platform_device_register_simple("omacvm-backlight", -1, NULL, 0);
	if (IS_ERR(omb_pdev))
		return PTR_ERR(omb_pdev);
	omb_bd = backlight_device_register("omacvm", &omb_pdev->dev, NULL, &omb_ops, &props);
	if (IS_ERR(omb_bd)) {
		platform_device_unregister(omb_pdev);
		return PTR_ERR(omb_bd);
	}
	return 0;
}

static void __exit omb_exit(void)
{
	backlight_device_unregister(omb_bd);
	platform_device_unregister(omb_pdev);
}

module_init(omb_init);
module_exit(omb_exit);
MODULE_DESCRIPTION("The Mac's display brightness as a backlight in an OmacVM.app VM");
MODULE_LICENSE("GPL");
