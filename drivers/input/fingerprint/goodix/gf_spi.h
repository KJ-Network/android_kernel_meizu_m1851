/*
 * driver definition for sensor driver
 *
 * Coypright (c) 2017 Goodix
 * Copyright (C) 2026 Kopsources.ORG
 */
#ifndef GOODIX_TEE_H
#define GOODIX_TEE_H

#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/types.h>

#define GF_COMPATIBLE "goodix,fingerprint"
#define GF_DEVICE     "goodix_fp"
#define GF_CLASS      "goodix_fp"
#define GF_NODE       "goodix_fp"
#define GF_CHRDEV     "goodix_fp_spi"
#define GF_NETLINK    25
#define GF_NET_MAX    32

enum gf_nav_event {
	GF_NAV_NONE = 0,
	GF_NAV_FINGER_UP,
	GF_NAV_FINGER_DOWN,
	GF_NAV_UP,
	GF_NAV_DOWN,
	GF_NAV_LEFT,
	GF_NAV_RIGHT,
	GF_NAV_CLICK,
	GF_NAV_HEAVY,
	GF_NAV_LONG_PRESS,
	GF_NAV_DOUBLE_CLICK,
};

enum gf_key_event {
	GF_KEY_NONE = 0,
	GF_KEY_HOME,
	GF_KEY_POWER,
	GF_KEY_MENU,
	GF_KEY_BACK,
	GF_KEY_CAMERA,
	GF_KEY_LONG_PRESS,
};

struct gf_key {
	__u32 key;
	__u32 value;
};

struct gf_chip_info {
	__u8 vendor_id;
	__u8 mode;
	__u8 operation;
	__u8 reserved[5];
};

#define GF_IOC_MAGIC            'g'
#define GF_IOC_INIT             _IOR(GF_IOC_MAGIC, 0, __u8)
#define GF_IOC_EXIT             _IO(GF_IOC_MAGIC, 1)
#define GF_IOC_RESET            _IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ       _IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ      _IO(GF_IOC_MAGIC, 4)
#define GF_IOC_ENABLE_SPI_CLK   _IOW(GF_IOC_MAGIC, 5, __u32)
#define GF_IOC_DISABLE_SPI_CLK  _IO(GF_IOC_MAGIC, 6)
#define GF_IOC_ENABLE_POWER     _IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER    _IO(GF_IOC_MAGIC, 8)
#define GF_IOC_INPUT_KEY_EVENT  _IOW(GF_IOC_MAGIC, 9, struct gf_key)
#define GF_IOC_ENTER_SLEEP_MODE _IO(GF_IOC_MAGIC, 10)
#define GF_IOC_GET_FW_INFO      _IOR(GF_IOC_MAGIC, 11, __u8)
#define GF_IOC_REMOVE           _IO(GF_IOC_MAGIC, 12)
#define GF_IOC_CHIP_INFO        _IOW(GF_IOC_MAGIC, 13, struct gf_chip_info)
#define GF_IOC_NAV_EVENT        _IOW(GF_IOC_MAGIC, 14, __u32)

#define GF_NET_EVENT_IRQ        1
#define GF_NET_EVENT_FB_BLACK   2
#define GF_NET_EVENT_FB_UNBLACK 3

struct gf_device {
	struct platform_device *pdev;
	struct input_dev *input;
	struct notifier_block fb_notifier;
	struct wakeup_source *wakeup;
	int irq_gpio;
	int reset_gpio;
	int irq;
	int users;
	bool irq_enabled;
	bool device_available;
	bool fb_black;
	bool fb_registered;
	bool irq_wake_enabled;
	dev_t devt;
	struct device *char_dev;
};

extern struct gf_device *gf_data;
extern struct class *gf_class;
extern int gf_major;

int gf_parse_dts(struct gf_device *data);
void gf_cleanup(struct gf_device *data);
int gf_power_on(struct gf_device *data);
int gf_power_off(struct gf_device *data);
int gf_hw_reset(struct gf_device *data, unsigned int delay_ms);
int gf_irq_num(struct gf_device *data);
void sendnlmsg(u8 event);
int netlink_init(void);
void netlink_exit(void);

#endif
