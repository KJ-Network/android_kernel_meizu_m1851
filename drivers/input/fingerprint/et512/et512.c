/*
 * Copyright (C) 2007-2016 Egis Technology Inc.
 * Copyright (C) 2026 Kopsources.ORG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#include "et512.h"

#define ET512_BUFFER_SIZE	4096
#define ET512_WAKE_JIFFIES	150
#define ET512_NOTIFY_EVENT	0xbeef

static DEFINE_MUTEX(et512_device_lock);
static DEFINE_MUTEX(fps_notifier_lock);
static BLOCKING_NOTIFIER_HEAD(fps_notifier_list);
static struct et512_data *et512_device;
static struct class *et512_class;
static unsigned long fps_sensor;
static unsigned int fps_clients;
static int fps_state;

int FPS_register_notifier(struct notifier_block *nb, unsigned long sensor,
			  bool report)
{
	int error;
	int state;

	if (!nb)
		return -EINVAL;
	mutex_lock(&fps_notifier_lock);
	fps_sensor = sensor;
	error = blocking_notifier_chain_register(&fps_notifier_list, nb);
	if (!error)
		fps_clients++;
	state = fps_state;
	mutex_unlock(&fps_notifier_lock);
	if (!error && report)
		blocking_notifier_call_chain(&fps_notifier_list, sensor,
					     &state);
	return error;
}
EXPORT_SYMBOL(FPS_register_notifier);

int FPS_unregister_notifier(struct notifier_block *nb, unsigned long sensor)
{
	int error;

	if (!nb)
		return -EINVAL;
	mutex_lock(&fps_notifier_lock);
	error = blocking_notifier_chain_unregister(&fps_notifier_list, nb);
	if (!error && fps_clients && !--fps_clients)
		fps_sensor = 0;
	mutex_unlock(&fps_notifier_lock);
	return error;
}
EXPORT_SYMBOL(FPS_unregister_notifier);

void FPS_notify(unsigned long event, int state)
{
	bool notify = false;

	mutex_lock(&fps_notifier_lock);
	if (fps_sensor && fps_state != state) {
		fps_state = state;
		notify = true;
	}
	mutex_unlock(&fps_notifier_lock);
	if (notify)
		blocking_notifier_call_chain(&fps_notifier_list, event, &state);
}
EXPORT_SYMBOL(FPS_notify);

static void et512_reset(struct et512_data *data)
{
	gpio_set_value(data->reset_gpio, 0);
	msleep(30);
	gpio_set_value(data->reset_gpio, 1);
	msleep(20);
}

static void et512_irq_timer(unsigned long arg)
{
	struct et512_data *data = (struct et512_data *)arg;

	if (data->irq_count >= data->detect_threshold)
		WRITE_ONCE(data->finger_on, true);
	data->irq_count = 0;
	wake_up_interruptible(&data->irq_waitq);
}

static irqreturn_t et512_edge_irq(int irq, void *arg)
{
	struct et512_data *data = arg;

	if (!data->irq_count)
		mod_timer(&data->irq_timer,
			  jiffies + msecs_to_jiffies(data->detect_period));
	data->irq_count++;
	if (data->wakeup)
		__pm_wakeup_event(data->wakeup,
				  jiffies_to_msecs(ET512_WAKE_JIFFIES));
	return IRQ_HANDLED;
}

static irqreturn_t et512_level_irq(int irq, void *arg)
{
	struct et512_data *data = arg;

	WRITE_ONCE(data->finger_on, true);
	disable_irq_nosync(irq);
	WRITE_ONCE(data->irq_enabled, false);
	wake_up_interruptible(&data->irq_waitq);
	if (data->wakeup)
		__pm_wakeup_event(data->wakeup,
				  jiffies_to_msecs(ET512_WAKE_JIFFIES));
	return IRQ_HANDLED;
}

static int et512_interrupt_init(struct et512_data *data,
				int mode, int period, int threshold)
{
	irq_handler_t handler;
	unsigned long flags;
	int error;

	data->detect_period = period;
	data->detect_threshold = threshold;
	data->irq_count = 0;
	data->finger_on = false;
	if (data->irq_requested) {
		if (!data->irq_enabled) {
			enable_irq(data->irq);
			data->irq_enabled = true;
		}
		return 0;
	}

	switch (mode) {
	case ET512_EDGE_FALLING:
		handler = et512_edge_irq;
		flags = IRQF_TRIGGER_FALLING;
		break;
	case ET512_EDGE_RISING:
		handler = et512_edge_irq;
		flags = IRQF_TRIGGER_RISING;
		break;
	case ET512_LEVEL_LOW:
		handler = et512_level_irq;
		flags = IRQF_TRIGGER_LOW;
		break;
	case ET512_LEVEL_HIGH:
		handler = et512_level_irq;
		flags = IRQF_TRIGGER_HIGH;
		break;
	default:
		return -EINVAL;
	}

	error = request_irq(data->irq, handler, flags, "fp_detect-eint", data);
	if (error)
		return error;
	data->irq_requested = true;
	data->irq_enabled = true;
	error = enable_irq_wake(data->irq);
	if (!error)
		data->irq_wake_enabled = true;
	return 0;
}

static void et512_interrupt_close(struct et512_data *data)
{
	WRITE_ONCE(data->finger_on, false);
	if (data->irq_requested && data->irq_enabled) {
		disable_irq_nosync(data->irq);
		data->irq_enabled = false;
	}
	del_timer_sync(&data->irq_timer);
}

static void et512_interrupt_abort(struct et512_data *data)
{
	WRITE_ONCE(data->finger_on, false);
	wake_up_interruptible(&data->irq_waitq);
}

static unsigned int et512_poll(struct file *file, poll_table *wait)
{
	struct et512_data *data = file->private_data;

	if (!data)
		return POLLERR;
	poll_wait(file, &data->irq_waitq, wait);
	return READ_ONCE(data->finger_on) ? POLLIN | POLLRDNORM : 0;
}

static ssize_t et512_read(struct file *file, char __user *buf, size_t count,
			  loff_t *offset)
{
	return 0;
}

static ssize_t et512_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *offset)
{
	return 0;
}

static long et512_ioctl(struct file *file, unsigned int command,
			unsigned long argument)
{
	struct et512_data *data = file->private_data;
	struct et512_ioc_irq config;
	long error = 0;

	if (!data || !READ_ONCE(data->present))
		return -ENODEV;
	mutex_lock(&data->irq_lock);
	switch (command) {
	case FP_SENSOR_RESET:
		et512_reset(data);
		break;
	case INT_TRIGGER_INIT:
		if (copy_from_user(&config, (void __user *)argument,
				   sizeof(config))) {
			error = -EFAULT;
			break;
		}
		error = et512_interrupt_init(data, config.mode,
					     config.detect_period,
					     config.detect_threshold);
		break;
	case INT_TRIGGER_CLOSE:
		et512_interrupt_close(data);
		break;
	case INT_TRIGGER_ABORT:
		et512_interrupt_abort(data);
		break;
	default:
		error = -ENOTTY;
		break;
	}
	mutex_unlock(&data->irq_lock);
	return error;
}

#ifdef CONFIG_COMPAT
static long et512_compat_ioctl(struct file *file, unsigned int command,
			       unsigned long argument)
{
	return et512_ioctl(file, command,
			   (unsigned long)compat_ptr(argument));
}
#endif

static int et512_open(struct inode *inode, struct file *file)
{
	struct et512_data *data;
	int error = 0;

	mutex_lock(&et512_device_lock);
	data = et512_device;
	if (!data || !data->present) {
		error = -ENXIO;
		goto out;
	}
	if (!data->buffer) {
		data->buffer = kmalloc(ET512_BUFFER_SIZE, GFP_KERNEL);
		if (!data->buffer) {
			error = -ENOMEM;
			goto out;
		}
	}
	data->users++;
	file->private_data = data;
	error = nonseekable_open(inode, file);
out:
	mutex_unlock(&et512_device_lock);
	return error;
}

static int et512_release(struct inode *inode, struct file *file)
{
	struct et512_data *data = file->private_data;
	bool free_data = false;

	if (!data)
		return 0;
	mutex_lock(&et512_device_lock);
	file->private_data = NULL;
	if (data->users)
		data->users--;
	if (!data->users) {
		kfree(data->buffer);
		data->buffer = NULL;
		free_data = !data->present;
	}
	mutex_unlock(&et512_device_lock);
	if (free_data)
		kfree(data);
	return 0;
}

static const struct file_operations et512_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read = et512_read,
	.write = et512_write,
	.poll = et512_poll,
	.unlocked_ioctl = et512_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = et512_compat_ioctl,
#endif
	.open = et512_open,
	.release = et512_release,
};

static ssize_t etspi_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	FPS_notify(ET512_NOTIFY_EVENT, buf[0] == '1');
	return 1;
}

static DEVICE_ATTR(etspi_enable, S_IWUSR | S_IWGRP, NULL,
		   etspi_enable_store);

static struct attribute *et512_attributes[] = {
	&dev_attr_etspi_enable.attr,
	NULL,
};

static const struct attribute_group et512_attribute_group = {
	.attrs = et512_attributes,
};

static int et512_enable_supply(struct et512_data *data)
{
	int error;

	data->vdd = devm_regulator_get_optional(&data->pdev->dev, "vdd_fp");
	if (IS_ERR(data->vdd)) {
		error = PTR_ERR(data->vdd);
		data->vdd = NULL;
		return error == -ENODEV ? 0 : error;
	}
	if (regulator_count_voltages(data->vdd) > 0) {
		error = regulator_set_voltage(data->vdd, 2850000, 2850000);
		if (error)
			return error;
	}
	error = regulator_enable(data->vdd);
	if (!error)
		data->vdd_enabled = true;
	return error;
}

static int et512_check_sensor_id(struct et512_data *data)
{
	struct device *dev = &data->pdev->dev;
	int error;

	data->id_gpio = of_get_named_gpio(dev->of_node, "finger,gpio_id", 0);
	if (!gpio_is_valid(data->id_gpio))
		return data->id_gpio == -ENOENT ? 0 : data->id_gpio;
	error = devm_gpio_request_one(dev, data->id_gpio, GPIOF_IN,
				      "finger_id");
	if (error)
		return error;
	if (!gpio_get_value(data->id_gpio)) {
		dev_info(dev, "Goodix sensor ID, skipping ET512\n");
		return -ENODEV;
	}
	return 0;
}

static int et512_parse_dt(struct et512_data *data)
{
	struct device *dev = &data->pdev->dev;
	int error;

	data->reset_gpio = of_get_named_gpio(dev->of_node,
					    "egistec,gpio_rst", 0);
	data->irq_gpio = of_get_named_gpio(dev->of_node,
					  "egistec,gpio_irq", 0);
	if (!gpio_is_valid(data->reset_gpio) ||
	    !gpio_is_valid(data->irq_gpio))
		return -EINVAL;
	error = devm_gpio_request_one(dev, data->reset_gpio,
				      GPIOF_OUT_INIT_HIGH, "et512_reset");
	if (error)
		return error;
	error = devm_gpio_request_one(dev, data->irq_gpio, GPIOF_IN,
				      "et512_irq");
	if (error)
		return error;
	data->irq = gpio_to_irq(data->irq_gpio);
	return data->irq < 0 ? data->irq : 0;
}

static int et512_create_chardev(struct et512_data *data)
{
	int error;

	error = register_chrdev(ET512_MAJOR, ET512_NAME, &et512_fops);
	if (error < 0)
		return error;
	et512_class = class_create(THIS_MODULE, ET512_NAME);
	if (IS_ERR(et512_class)) {
		error = PTR_ERR(et512_class);
		et512_class = NULL;
		unregister_chrdev(ET512_MAJOR, ET512_NAME);
		return error;
	}
	data->char_dev = device_create(et512_class, &data->pdev->dev,
				       MKDEV(ET512_MAJOR, 0), data,
				       ET512_NODE);
	if (IS_ERR(data->char_dev)) {
		error = PTR_ERR(data->char_dev);
		data->char_dev = NULL;
		class_destroy(et512_class);
		et512_class = NULL;
		unregister_chrdev(ET512_MAJOR, ET512_NAME);
		return error;
	}
	return 0;
}

static void et512_destroy_chardev(struct et512_data *data)
{
	if (data->char_dev)
		device_destroy(et512_class, MKDEV(ET512_MAJOR, 0));
	data->char_dev = NULL;
	if (et512_class) {
		class_destroy(et512_class);
		et512_class = NULL;
	}
	unregister_chrdev(ET512_MAJOR, ET512_NAME);
}

static int et512_probe(struct platform_device *pdev)
{
	struct et512_data *data;
	int error;

	mutex_lock(&et512_device_lock);
	if (et512_device) {
		mutex_unlock(&et512_device_lock);
		return -EBUSY;
	}
	mutex_unlock(&et512_device_lock);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->pdev = pdev;
	mutex_init(&data->irq_lock);
	init_waitqueue_head(&data->irq_waitq);
	setup_timer(&data->irq_timer, et512_irq_timer, (unsigned long)data);

	error = et512_enable_supply(data);
	if (error)
		goto err_free;
	error = et512_check_sensor_id(data);
	if (error)
		goto err_supply;
	error = et512_parse_dt(data);
	if (error)
		goto err_supply;
	error = et512_create_chardev(data);
	if (error)
		goto err_supply;
	data->wakeup = wakeup_source_register("et512_wake_lock");
	if (!data->wakeup) {
		error = -ENOMEM;
		goto err_chardev;
	}
	error = et512_navi_init(data);
	if (error)
		goto err_wakeup;
	error = sysfs_create_group(&pdev->dev.kobj, &et512_attribute_group);
	if (error)
		goto err_navi;

	et512_reset(data);
	data->present = true;
	platform_set_drvdata(pdev, data);
	mutex_lock(&et512_device_lock);
	et512_device = data;
	mutex_unlock(&et512_device_lock);
	return 0;

err_navi:
	et512_navi_destroy(data);
err_wakeup:
	wakeup_source_unregister(data->wakeup);
	data->wakeup = NULL;
err_chardev:
	et512_destroy_chardev(data);
err_supply:
	if (data->vdd_enabled)
		regulator_disable(data->vdd);
err_free:
	kfree(data);
	return error;
}

static int et512_remove(struct platform_device *pdev)
{
	struct et512_data *data = platform_get_drvdata(pdev);
	bool free_data;

	if (!data)
		return 0;
	mutex_lock(&et512_device_lock);
	et512_device = NULL;
	data->present = false;
	sysfs_remove_group(&pdev->dev.kobj, &et512_attribute_group);
	et512_navi_destroy(data);
	mutex_lock(&data->irq_lock);
	if (data->irq_requested) {
		if (data->irq_wake_enabled)
			disable_irq_wake(data->irq);
		free_irq(data->irq, data);
		data->irq_requested = false;
	}
	del_timer_sync(&data->irq_timer);
	mutex_unlock(&data->irq_lock);
	if (data->wakeup)
		wakeup_source_unregister(data->wakeup);
	if (data->vdd_enabled)
		regulator_disable(data->vdd);
	et512_destroy_chardev(data);
	platform_set_drvdata(pdev, NULL);
	free_data = !data->users;
	mutex_unlock(&et512_device_lock);
	if (free_data)
		kfree(data);
	return 0;
}

static const struct of_device_id et512_of_match[] = {
	{ .compatible = "egistec,et512" },
	{ }
};
MODULE_DEVICE_TABLE(of, et512_of_match);

static struct platform_driver et512_driver = {
	.probe = et512_probe,
	.remove = et512_remove,
	.driver = {
		.name = "et512",
		.of_match_table = et512_of_match,
	},
};

module_platform_driver(et512_driver);

MODULE_AUTHOR("Egis Technology Inc.");
MODULE_DESCRIPTION("Egis ET512 fingerprint sensor driver");
MODULE_LICENSE("GPL");
