/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Kopsources.ORG
 */
#ifndef _LINUX_FINGERPRINT_ID_H
#define _LINUX_FINGERPRINT_ID_H

struct device;

enum fingerprint_sensor_id {
	FINGERPRINT_ID_GOODIX = 0,
	FINGERPRINT_ID_ET512 = 1,
};

int fingerprint_id_match(struct device *dev, enum fingerprint_sensor_id id);

#endif
