/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_M1851_BOOT_TIMEOUT_H
#define _LINUX_M1851_BOOT_TIMEOUT_H

#include <linux/types.h>

#ifdef CONFIG_M1851_BOOT_TIMEOUT
void m1851_boot_timeout_start(void);
void m1851_boot_timeout_init_timer(void);
void m1851_boot_timeout_disarm(void);
bool m1851_boot_timeout_owns_watchdog(void);
#else
static inline void m1851_boot_timeout_start(void)
{
}

static inline void m1851_boot_timeout_init_timer(void)
{
}

static inline void m1851_boot_timeout_disarm(void)
{
}

static inline bool m1851_boot_timeout_owns_watchdog(void)
{
	return false;
}
#endif

#endif /* _LINUX_M1851_BOOT_TIMEOUT_H */
