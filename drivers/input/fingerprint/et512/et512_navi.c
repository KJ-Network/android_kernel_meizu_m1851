/*
 * Copyright (C) 2007-2016 Egis Technology Inc.
 * Copyright (C) 2026 Kopsources.ORG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "et512.h"

#define ET512_KEY_HOLD		617
#define ET512_KEY_YPLUS		618
#define ET512_KEY_YMINUS	619
#define ET512_KEY_XPLUS		620
#define ET512_KEY_XMINUS	621

enum et512_navi_event {
	ET512_NAVI_CANCEL,
	ET512_NAVI_ON,
	ET512_NAVI_OFF,
	ET512_NAVI_SWIPE,
	ET512_NAVI_UP,
	ET512_NAVI_DOWN,
	ET512_NAVI_RIGHT,
	ET512_NAVI_LEFT,
};

struct et512_navi_cmd {
	u8 command;
	struct list_head entry;
};

static void et512_send_key(struct et512_data *data, unsigned int code)
{
	input_report_key(data->input, code, 1);
	input_sync(data->input);
	input_report_key(data->input, code, 0);
	input_sync(data->input);
}

static void et512_long_touch(unsigned long arg)
{
	struct et512_data *data = (struct et512_data *)arg;

	if (!READ_ONCE(data->nav_event_raised)) {
		WRITE_ONCE(data->nav_event_raised, true);
		et512_send_key(data, ET512_KEY_HOLD);
	}
}

static void et512_translate_command(struct et512_data *data, u8 command)
{
	switch (command) {
	case ET512_NAVI_CANCEL:
		data->nav_event_raised = true;
		del_timer(&data->nav_timer);
		break;
	case ET512_NAVI_ON:
		data->nav_event_raised = false;
		mod_timer(&data->nav_timer, jiffies + 50);
		break;
	case ET512_NAVI_OFF:
		if (!data->nav_event_raised) {
			data->nav_event_raised = true;
			et512_send_key(data, KEY_BACK);
		}
		del_timer(&data->nav_timer);
		break;
	case ET512_NAVI_UP:
	case ET512_NAVI_DOWN:
		if (!data->nav_event_raised)
			data->nav_event_raised = true;
		break;
	case ET512_NAVI_RIGHT:
		if (!data->nav_event_raised) {
			data->nav_event_raised = true;
			et512_send_key(data, ET512_KEY_YMINUS);
		}
		break;
	case ET512_NAVI_LEFT:
		if (!data->nav_event_raised) {
			data->nav_event_raised = true;
			et512_send_key(data, ET512_KEY_YPLUS);
		}
		break;
	default:
		break;
	}
}

static int et512_navi_thread(void *arg)
{
	struct et512_data *data = arg;
	struct et512_navi_cmd *command;
	struct et512_navi_cmd *next;
	LIST_HEAD(queue);

	set_user_nice(current, -20);
	while (!kthread_should_stop()) {
		wait_event_interruptible(data->nav_waitq,
					 !list_empty(&data->nav_queue) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		mutex_lock(&data->nav_lock);
		list_splice_init(&data->nav_queue, &queue);
		mutex_unlock(&data->nav_lock);

		list_for_each_entry_safe(command, next, &queue, entry) {
			et512_translate_command(data, command->command);
			list_del(&command->entry);
			kfree(command);
		}
	}

	return 0;
}

static ssize_t navigation_event_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct et512_data *data = dev_get_drvdata(dev);
	struct et512_navi_cmd *command;

	if (!data || !data->input || !count)
		return -ENODEV;
	command = kmalloc(sizeof(*command), GFP_KERNEL);
	if (!command)
		return -ENOMEM;
	command->command = buf[0];
	mutex_lock(&data->nav_lock);
	list_add_tail(&command->entry, &data->nav_queue);
	mutex_unlock(&data->nav_lock);
	wake_up_interruptible(&data->nav_waitq);
	return count;
}

static ssize_t navigation_enable_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct et512_data *data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%s",
			 data->navigation_enabled ? "enable" : "disable");
}

static ssize_t navigation_enable_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct et512_data *data = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", 6))
		data->navigation_enabled = true;
	else if (!strncmp(buf, "disable", 7))
		data->navigation_enabled = false;
	return count;
}

static DEVICE_ATTR(navigation_event, S_IWUSR, NULL, navigation_event_store);
static DEVICE_ATTR(navigation_enable, S_IRUSR | S_IWUSR,
		   navigation_enable_show, navigation_enable_store);

static struct attribute *et512_navi_attributes[] = {
	&dev_attr_navigation_event.attr,
	&dev_attr_navigation_enable.attr,
	NULL,
};

static const struct attribute_group et512_navi_attribute_group = {
	.attrs = et512_navi_attributes,
};

int et512_navi_init(struct et512_data *data)
{
	int error;

	INIT_LIST_HEAD(&data->nav_queue);
	init_waitqueue_head(&data->nav_waitq);
	mutex_init(&data->nav_lock);
	setup_timer(&data->nav_timer, et512_long_touch,
		    (unsigned long)data);
	data->navigation_enabled = true;
	data->nav_event_raised = true;

	data->input = input_allocate_device();
	if (!data->input)
		return -ENOMEM;
	data->input->name = "uinput-egis";
	input_set_capability(data->input, EV_KEY, KEY_BACK);
	input_set_capability(data->input, EV_KEY, KEY_DELETE);
	input_set_capability(data->input, EV_KEY, ET512_KEY_HOLD);
	input_set_capability(data->input, EV_KEY, ET512_KEY_XMINUS);
	input_set_capability(data->input, EV_KEY, ET512_KEY_XPLUS);
	input_set_capability(data->input, EV_KEY, ET512_KEY_YMINUS);
	input_set_capability(data->input, EV_KEY, ET512_KEY_YPLUS);
	error = input_register_device(data->input);
	if (error) {
		input_free_device(data->input);
		data->input = NULL;
		return error;
	}

	data->nav_thread = kthread_run(et512_navi_thread, data, "nav_thread");
	if (IS_ERR(data->nav_thread)) {
		error = PTR_ERR(data->nav_thread);
		data->nav_thread = NULL;
		goto err_input;
	}

	data->nav_pdev = platform_device_alloc("egis_input", -1);
	if (!data->nav_pdev) {
		error = -ENOMEM;
		goto err_thread;
	}
	error = platform_device_add(data->nav_pdev);
	if (error)
		goto err_pdev;
	dev_set_drvdata(&data->nav_pdev->dev, data);
	error = sysfs_create_group(&data->nav_pdev->dev.kobj,
				   &et512_navi_attribute_group);
	if (error)
		goto err_pdev_added;
	return 0;

err_pdev_added:
	platform_device_del(data->nav_pdev);
err_pdev:
	platform_device_put(data->nav_pdev);
	data->nav_pdev = NULL;
err_thread:
	kthread_stop(data->nav_thread);
	data->nav_thread = NULL;
err_input:
	input_unregister_device(data->input);
	data->input = NULL;
	del_timer_sync(&data->nav_timer);
	return error;
}

void et512_navi_destroy(struct et512_data *data)
{
	struct et512_navi_cmd *command;
	struct et512_navi_cmd *next;

	if (data->nav_pdev) {
		sysfs_remove_group(&data->nav_pdev->dev.kobj,
				   &et512_navi_attribute_group);
		platform_device_unregister(data->nav_pdev);
		data->nav_pdev = NULL;
	}
	if (data->nav_thread) {
		kthread_stop(data->nav_thread);
		data->nav_thread = NULL;
	}
	del_timer_sync(&data->nav_timer);
	mutex_lock(&data->nav_lock);
	list_for_each_entry_safe(command, next, &data->nav_queue, entry) {
		list_del(&command->entry);
		kfree(command);
	}
	mutex_unlock(&data->nav_lock);
	if (data->input) {
		input_unregister_device(data->input);
		data->input = NULL;
	}
}
