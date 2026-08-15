/*
 * Copyright (C) 2007-2016 Egis Technology Inc.
 * Copyright (C) 2026 Kopsources.ORG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ET512_H_
#define _ET512_H_

#include <linux/device.h>
#include <linux/input.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/timer.h>
#include <linux/wait.h>

#define ET512_MAJOR		100
#define ET512_MINORS		256
#define ET512_NAME		"gis_et512"
#define ET512_NODE		"esfp0"

#define FP_SENSOR_RESET		0x04
#define INT_TRIGGER_INIT	0xa4
#define INT_TRIGGER_CLOSE	0xa5
#define INT_TRIGGER_ABORT	0xa8

enum et512_irq_mode {
	ET512_EDGE_FALLING,
	ET512_EDGE_RISING,
	ET512_LEVEL_LOW,
	ET512_LEVEL_HIGH,
};

struct et512_ioc_irq {
	int mode;
	int detect_period;
	int detect_threshold;
};

struct et512_data {
	struct platform_device *pdev;
	struct platform_device *nav_pdev;
	struct device *char_dev;
	struct input_dev *input;
	struct task_struct *nav_thread;
	struct wakeup_source *wakeup;
	struct regulator *vdd;
	struct timer_list irq_timer;
	struct timer_list nav_timer;
	wait_queue_head_t irq_waitq;
	wait_queue_head_t nav_waitq;
	struct list_head nav_queue;
	struct mutex irq_lock; /* serializes interrupt setup and teardown */
	struct mutex nav_lock; /* protects the navigation command queue */
	int reset_gpio;
	int irq_gpio;
	int id_gpio;
	int irq;
	int detect_period;
	int detect_threshold;
	int irq_count;
	unsigned int users;
	u8 *buffer;
	bool present;
	bool vdd_enabled;
	bool irq_requested;
	bool irq_enabled;
	bool irq_wake_enabled;
	bool finger_on;
	bool navigation_enabled;
	bool nav_event_raised;
};

int et512_navi_init(struct et512_data *data);
void et512_navi_destroy(struct et512_data *data);

int FPS_register_notifier(struct notifier_block *nb, unsigned long sensor,
			  bool report);
int FPS_unregister_notifier(struct notifier_block *nb, unsigned long sensor);
void FPS_notify(unsigned long event, int state);

#endif
