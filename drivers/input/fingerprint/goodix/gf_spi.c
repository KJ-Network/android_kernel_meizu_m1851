/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
 * Copyright (C) 2018 XiaoMi, Inc.
 * Copyright (C) 2026 Kopsources.ORG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/compat.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/fingerprint_id.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "gf_spi.h"

static DEFINE_MUTEX(gf_lock);
struct gf_device *gf_data;
struct class *gf_class;
int gf_major;

static void gf_set_irq(struct gf_device *data, bool enable)
{
	if (enable && !data->irq_enabled) {
		enable_irq(data->irq);
		data->irq_enabled = true;
	} else if (!enable && data->irq_enabled) {
		data->irq_enabled = false;
		disable_irq(data->irq);
	}
}

static irqreturn_t gf_irq(int irq, void *arg)
{
	struct gf_device *data = arg;

	if (data->wakeup)
		__pm_wakeup_event(data->wakeup, jiffies_to_msecs(50));
	sendnlmsg(GF_NET_EVENT_IRQ);
	return IRQ_HANDLED;
}

static void gf_key_event(struct gf_device *data, const struct gf_key *key)
{
	unsigned int code;

	switch (key->key) {
	case GF_KEY_POWER:
		code = KEY_POWER;
		break;
	case GF_KEY_CAMERA:
		code = KEY_CAMERA;
		break;
	default:
		return;
	}
	if (!key->value)
		return;
	input_report_key(data->input, code, 1);
	input_sync(data->input);
	input_report_key(data->input, code, 0);
	input_sync(data->input);
}

static void gf_nav_event(struct gf_device *data, u32 event)
{
	static const unsigned short codes[] = {
		[GF_NAV_UP] = KEY_UP, [GF_NAV_DOWN] = KEY_DOWN,
		[GF_NAV_LEFT] = KEY_LEFT, [GF_NAV_RIGHT] = KEY_RIGHT,
		[GF_NAV_CLICK] = KEY_VOLUMEDOWN, [GF_NAV_HEAVY] = KEY_CHAT,
		[GF_NAV_LONG_PRESS] = KEY_SEARCH,
		[GF_NAV_DOUBLE_CLICK] = KEY_VOLUMEUP,
	};

	if (event >= ARRAY_SIZE(codes) || event == GF_NAV_FINGER_UP ||
	    event == GF_NAV_FINGER_DOWN || !codes[event])
		return;
	input_report_key(data->input, codes[event], 1);
	input_sync(data->input);
	input_report_key(data->input, codes[event], 0);
	input_sync(data->input);
}

static long gf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct gf_device *data = gf_data;
	struct gf_chip_info info;
	struct gf_key key;
	u8 route = GF_NETLINK;
	u32 nav;

	if (!data)
		return -ENODEV;
	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENODEV;
	mutex_lock(&gf_lock);
	if (!data->device_available && cmd != GF_IOC_ENABLE_POWER &&
	    cmd != GF_IOC_DISABLE_POWER) {
		mutex_unlock(&gf_lock);
		return -ENODEV;
	}
	switch (cmd) {
	case GF_IOC_INIT:
		if (copy_to_user((void __user *)arg, &route, sizeof(route)))
			goto fault;
		break;
	case GF_IOC_ENABLE_IRQ:
		gf_set_irq(data, true);
		break;
	case GF_IOC_DISABLE_IRQ:
		gf_set_irq(data, false);
		break;
	case GF_IOC_RESET:
		gf_hw_reset(data, 3);
		break;
	case GF_IOC_ENABLE_POWER:
		gf_power_on(data);
		break;
	case GF_IOC_DISABLE_POWER:
		gf_power_off(data);
		break;
	case GF_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&key, (void __user *)arg, sizeof(key)))
			goto fault;
		gf_key_event(data, &key);
		break;
	case GF_IOC_NAV_EVENT:
		if (copy_from_user(&nav, (void __user *)arg, sizeof(nav)))
			goto fault;
		gf_nav_event(data, nav);
		break;
	case GF_IOC_CHIP_INFO:
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			goto fault;
		dev_info(&data->pdev->dev, "vendor_id=0x%x mode=0x%x operation=0x%x\n",
			 info.vendor_id, info.mode, info.operation);
		break;
	case GF_IOC_EXIT:
	case GF_IOC_ENABLE_SPI_CLK:
	case GF_IOC_DISABLE_SPI_CLK:
	case GF_IOC_ENTER_SLEEP_MODE:
	case GF_IOC_GET_FW_INFO:
	case GF_IOC_REMOVE:
		break;
	default:
		break;
	}
	mutex_unlock(&gf_lock);
	return 0;
fault:
	mutex_unlock(&gf_lock);
	return -EFAULT;
}

#ifdef CONFIG_COMPAT
static long gf_compat_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	return gf_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int gf_open(struct inode *inode, struct file *file)
{
	struct gf_device *data = gf_data;

	if (!data)
		return -ENODEV;
	mutex_lock(&gf_lock);
	data->users++;
	if (data->users == 1)
		gf_set_irq(data, true);
	gf_power_on(data);
	gf_hw_reset(data, 3);
	file->private_data = data;
	mutex_unlock(&gf_lock);
	return nonseekable_open(inode, file);
}

static int gf_release(struct inode *inode, struct file *file)
{
	struct gf_device *data = file->private_data;

	if (!data)
		return 0;
	mutex_lock(&gf_lock);
	if (data->users > 0)
		data->users--;
	if (!data->users) {
		gf_set_irq(data, false);
		gf_power_off(data);
	}
	file->private_data = NULL;
	mutex_unlock(&gf_lock);
	return 0;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.open = gf_open,
	.release = gf_release,
	.unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gf_compat_ioctl,
#endif
};

static int gf_fb_event(struct notifier_block *nb, unsigned long val, void *ptr)
{
	struct gf_device *data;
	struct fb_event *event = ptr;
	int blank;

	data = container_of(nb, struct gf_device, fb_notifier);

	if (val != FB_EARLY_EVENT_BLANK || !event || !event->data ||
	    !data->device_available)
		return 0;
	blank = *(int *)event->data;
	if (blank == FB_BLANK_POWERDOWN) {
		data->fb_black = true;
		sendnlmsg(GF_NET_EVENT_FB_BLACK);
	} else if (blank == FB_BLANK_UNBLANK) {
		data->fb_black = false;
		sendnlmsg(GF_NET_EVENT_FB_UNBLACK);
	}
	return NOTIFY_OK;
}

static int gf_probe(struct platform_device *pdev)
{
	struct gf_device *data;
	int ret;

	ret = fingerprint_id_match(&pdev->dev, FINGERPRINT_ID_GOODIX);
	if (ret)
		return ret;
	if (gf_data)
		return -EBUSY;
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->pdev = pdev;
	ret = gf_parse_dts(data);
	if (ret)
		return ret;
	data->input = devm_input_allocate_device(&pdev->dev);
	if (!data->input)
		return -ENOMEM;
	data->input->name = "fp-keys";
	input_set_capability(data->input, EV_KEY, KEY_HOME);
	input_set_capability(data->input, EV_KEY, KEY_POWER);
	input_set_capability(data->input, EV_KEY, KEY_MENU);
	input_set_capability(data->input, EV_KEY, KEY_BACK);
	input_set_capability(data->input, EV_KEY, KEY_CAMERA);
	input_set_capability(data->input, EV_KEY, KEY_UP);
	input_set_capability(data->input, EV_KEY, KEY_DOWN);
	input_set_capability(data->input, EV_KEY, KEY_LEFT);
	input_set_capability(data->input, EV_KEY, KEY_RIGHT);
	input_set_capability(data->input, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(data->input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(data->input, EV_KEY, KEY_SEARCH);
	input_set_capability(data->input, EV_KEY, KEY_CHAT);
	ret = input_register_device(data->input);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(&pdev->dev, data->irq, NULL, gf_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"gf", data);
	if (ret)
		return ret;
	data->irq_enabled = true;
	ret = enable_irq_wake(data->irq);
	if (!ret)
		data->irq_wake_enabled = true;
	gf_set_irq(data, false);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	data->wakeup = wakeup_source_register(&pdev->dev, "fp_wakelock");
#else
	data->wakeup = wakeup_source_register("fp_wakelock");
#endif
	data->fb_notifier.notifier_call = gf_fb_event;
	ret = fb_register_client(&data->fb_notifier);
	if (ret)
		goto err_wakeup;
	data->fb_registered = true;
	data->devt = MKDEV(gf_major, 0);
	data->char_dev = device_create(gf_class, &pdev->dev, data->devt,
				       data, GF_NODE);
	if (IS_ERR(data->char_dev)) {
		ret = PTR_ERR(data->char_dev);
		data->char_dev = NULL;
		goto err_fb;
	}
	platform_set_drvdata(pdev, data);
	gf_data = data;
	return 0;
err_fb:
	fb_unregister_client(&data->fb_notifier);
err_wakeup:
	gf_cleanup(data);
	return ret;
}

static int gf_remove(struct platform_device *pdev)
{
	struct gf_device *data = platform_get_drvdata(pdev);

	if (!data)
		return 0;
	if (data->char_dev)
		device_destroy(gf_class, data->devt);
	if (data->fb_registered)
		fb_unregister_client(&data->fb_notifier);
	gf_cleanup(data);
	gf_data = NULL;
	return 0;
}

static const struct of_device_id gf_of_match[] = {
	{ .compatible = GF_COMPATIBLE }, { }
};
MODULE_DEVICE_TABLE(of, gf_of_match);

static struct platform_driver gf_driver = {
	.probe = gf_probe,
	.remove = gf_remove,
	.driver = {
		.name = GF_DEVICE,
		.of_match_table = gf_of_match,
	},
};

static int __init gf_init(void)
{
	int ret;

	ret = register_chrdev(0, GF_CHRDEV, &gf_fops);
	if (ret < 0)
		return ret;
	gf_major = ret;
	gf_class = class_create(THIS_MODULE, GF_CLASS);
	if (IS_ERR(gf_class)) {
		unregister_chrdev(gf_major, GF_CHRDEV);
		return PTR_ERR(gf_class);
	}
	ret = netlink_init();
	if (ret)
		goto err_class;
	ret = platform_driver_register(&gf_driver);
	if (ret)
		netlink_exit();
	if (ret)
		goto err_class;
	return 0;
err_class:
	class_destroy(gf_class);
	unregister_chrdev(gf_major, GF_CHRDEV);
	return ret;
}
module_init(gf_init);

static void __exit gf_exit(void)
{
	platform_driver_unregister(&gf_driver);
	netlink_exit();
	class_destroy(gf_class);
	unregister_chrdev(gf_major, GF_CHRDEV);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_DESCRIPTION("Goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
