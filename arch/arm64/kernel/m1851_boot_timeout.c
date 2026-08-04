// SPDX-License-Identifier: GPL-2.0
/*
 * Temporary early-boot timeout for Meizu m1851 bring-up.
 */

#define pr_fmt(fmt) "m1851-boot-timeout: " fmt

#include <linux/atomic.h>
#include <linux/console.h>
#include <linux/export.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/m1851_boot_timeout.h>
#include <linux/reboot.h>
#include <linux/sizes.h>

#define M1851_BOOT_TIMEOUT_SECONDS	120U
#define M1851_BOOT_HEARTBEAT_SECONDS	20U

/* SDM630/636 APSS watchdog, qcom,wdt@17817000. */
#define M1851_WDT_PHYS_BASE		0x17817000
#define M1851_WDT_SIZE			SZ_4K
#define M1851_WDT_MAX_TICKS		0x000fffffU
#define M1851_WDT_RST			0x04
#define M1851_WDT_EN			0x08
#define M1851_WDT_BARK_TIME		0x10
#define M1851_WDT_BITE_TIME		0x14

/* The first 256 KiB is a valid prefix of the 2 MiB console-ramoops zone. */
#define M1851_RAMOOPS_CONSOLE_PHYS	0xa0000000
#define M1851_EARLY_RAMOOPS_SIZE		SZ_256K
#define M1851_PERSISTENT_RAM_SIG		0x43474244
struct m1851_persistent_ram_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8 data[];
};

enum m1851_boot_timeout_state {
	M1851_BOOT_TIMEOUT_INACTIVE,
	M1851_BOOT_TIMEOUT_ARMED,
	M1851_BOOT_TIMEOUT_DISARMING,
	M1851_BOOT_TIMEOUT_DISARMED,
	M1851_BOOT_TIMEOUT_EXPIRED,
};

static atomic_t m1851_boot_timeout_state =
	ATOMIC_INIT(M1851_BOOT_TIMEOUT_INACTIVE);
static struct hrtimer m1851_boot_timeout_timer;
static void __iomem *m1851_boot_timeout_wdt_base;
static struct m1851_persistent_ram_buffer __iomem *m1851_early_ramoops;
static u64 m1851_boot_timeout_counter;
static u64 m1851_boot_timeout_frequency;
static bool m1851_early_console_registered;

static u64 m1851_boot_timeout_read_counter(void)
{
	u64 counter;

	asm volatile("mrs %0, cntvct_el0" : "=r" (counter));

	return counter;
}

static void m1851_boot_timeout_arm_wdt(void __iomem *base)
{
	if (!base)
		return;

	__raw_writel(0, base + M1851_WDT_EN);
	__raw_writel(1, base + M1851_WDT_RST);
	__raw_writel(M1851_WDT_MAX_TICKS, base + M1851_WDT_BARK_TIME);
	__raw_writel(M1851_WDT_MAX_TICKS, base + M1851_WDT_BITE_TIME);
	__raw_writel(1, base + M1851_WDT_EN);
	__raw_writel(1, base + M1851_WDT_RST);
	/* Ensure the complete watchdog configuration reaches the device. */
	mb();
}

static void m1851_boot_timeout_disable_wdt(void __iomem *base)
{
	if (!base)
		return;

	__raw_writel(0, base + M1851_WDT_EN);
	/* Confirm that the watchdog is disabled before returning. */
	mb();
}

static void m1851_early_console_write(struct console *console,
				      const char *text, unsigned int count)
{
	struct m1851_persistent_ram_buffer __iomem *buffer;
	const u32 capacity = M1851_EARLY_RAMOOPS_SIZE - sizeof(*buffer);
	u32 start, size, first;

	buffer = m1851_early_ramoops;
	if (!buffer || !count)
		return;

	start = readl_relaxed(&buffer->start);
	size = readl_relaxed(&buffer->size);
	if (start >= capacity || size > capacity) {
		start = 0;
		size = 0;
	}

	if (count > capacity) {
		text += count - capacity;
		count = capacity;
	}

	first = min(count, capacity - start);
	memcpy_toio(buffer->data + start, text, first);
	if (count != first)
		memcpy_toio(buffer->data, text + first, count - first);

	start += count;
	if (start >= capacity)
		start -= capacity;
	size = min_t(u32, capacity, size + count);

	writel_relaxed(start, &buffer->start);
	writel_relaxed(size, &buffer->size);
	/* Publish complete text and header updates before a possible reset. */
	wmb();
}

static struct console m1851_early_console = {
	.name = "m1851-pstore",
	.write = m1851_early_console_write,
	.flags = CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index = -1,
};

static void __init m1851_early_console_init(void)
{
	m1851_early_ramoops = early_ioremap(M1851_RAMOOPS_CONSOLE_PHYS,
					    M1851_EARLY_RAMOOPS_SIZE);
	if (!m1851_early_ramoops) {
		pr_warn("cannot map early console-ramoops window\n");
		return;
	}

	/* Discard stale data before replaying this kernel's printk buffer. */
	writel_relaxed(M1851_PERSISTENT_RAM_SIG,
		       &m1851_early_ramoops->sig);
	writel_relaxed(0, &m1851_early_ramoops->start);
	writel_relaxed(0, &m1851_early_ramoops->size);
	/* Publish the empty header before console replay begins. */
	wmb();

	register_console(&m1851_early_console);
	m1851_early_console_registered = true;
	pr_emerg("early persistent console active at phys 0x%llx\n",
		 (unsigned long long)M1851_RAMOOPS_CONSOLE_PHYS);
}

void __init m1851_boot_timeout_start(void)
{
	void __iomem *base;

	m1851_early_console_init();

	asm volatile("mrs %0, cntfrq_el0"
		     : "=r" (m1851_boot_timeout_frequency));
	if (!m1851_boot_timeout_frequency) {
		pr_emerg("architected counter has no frequency; timeout disabled\n");
		return;
	}

	m1851_boot_timeout_counter = m1851_boot_timeout_read_counter();
	atomic_set(&m1851_boot_timeout_state, M1851_BOOT_TIMEOUT_ARMED);

	/* A 20-bit counter gives about 32 seconds before hrtimers can pet it. */
	base = early_ioremap(M1851_WDT_PHYS_BASE, M1851_WDT_SIZE);
	if (!base) {
		pr_warn("cannot map APSS watchdog for early reset fallback\n");
		return;
	}
	m1851_boot_timeout_arm_wdt(base);
	early_iounmap(base, M1851_WDT_SIZE);
	pr_info("early hardware reset fallback armed at maximum count\n");
}

static u64 m1851_boot_timeout_elapsed_cycles(void)
{
	return m1851_boot_timeout_read_counter() - m1851_boot_timeout_counter;
}

static bool m1851_boot_timeout_deadline_reached(void)
{
	return m1851_boot_timeout_elapsed_cycles() >=
		m1851_boot_timeout_frequency * M1851_BOOT_TIMEOUT_SECONDS;
}

static u64 m1851_boot_timeout_next_interval_ns(void)
{
	u64 elapsed, timeout, remaining, interval;

	elapsed = m1851_boot_timeout_elapsed_cycles();
	timeout = m1851_boot_timeout_frequency * M1851_BOOT_TIMEOUT_SECONDS;
	remaining = elapsed < timeout ? timeout - elapsed : 0;
	interval = min_t(u64, remaining,
			 m1851_boot_timeout_frequency *
			 M1851_BOOT_HEARTBEAT_SECONDS);

	return div64_u64(interval * NSEC_PER_SEC,
			 m1851_boot_timeout_frequency);
}

static enum hrtimer_restart
m1851_boot_timeout_expired(struct hrtimer *timer)
{
	u64 next_interval_ns;

	if (atomic_read(&m1851_boot_timeout_state) !=
	    M1851_BOOT_TIMEOUT_ARMED)
		return HRTIMER_NORESTART;

	if (!m1851_boot_timeout_deadline_reached()) {
		if (m1851_boot_timeout_wdt_base)
			__raw_writel(1, m1851_boot_timeout_wdt_base +
					  M1851_WDT_RST);
		/* Make the pet visible before scheduling the next heartbeat. */
		wmb();
		next_interval_ns = m1851_boot_timeout_next_interval_ns();
		hrtimer_forward_now(timer, ns_to_ktime(next_interval_ns));
		return HRTIMER_RESTART;
	}

	if (atomic_cmpxchg(&m1851_boot_timeout_state,
			   M1851_BOOT_TIMEOUT_ARMED,
			   M1851_BOOT_TIMEOUT_EXPIRED) !=
			   M1851_BOOT_TIMEOUT_ARMED)
		return HRTIMER_NORESTART;

	pr_emerg("first display frame missing after %u seconds; restarting\n",
		 M1851_BOOT_TIMEOUT_SECONDS);

	/* Give the warm restart path a full hardware-fallback interval. */
	if (m1851_boot_timeout_wdt_base)
		__raw_writel(1, m1851_boot_timeout_wdt_base + M1851_WDT_RST);
	/* Flush the last printk and watchdog pet before entering restart code. */
	mb();
	emergency_restart();
	return HRTIMER_NORESTART;
}

void __init m1851_boot_timeout_init_timer(void)
{
	u64 elapsed, remaining_cycles, timeout_cycles, first_ns;

	if (atomic_read(&m1851_boot_timeout_state) !=
	    M1851_BOOT_TIMEOUT_ARMED)
		return;

	m1851_boot_timeout_wdt_base = ioremap(M1851_WDT_PHYS_BASE,
					      M1851_WDT_SIZE);
	if (!m1851_boot_timeout_wdt_base)
		pr_warn("cannot map APSS watchdog reset backstop\n");
	else
		m1851_boot_timeout_arm_wdt(m1851_boot_timeout_wdt_base);

	elapsed = m1851_boot_timeout_elapsed_cycles();
	timeout_cycles = m1851_boot_timeout_frequency *
			 M1851_BOOT_TIMEOUT_SECONDS;
	remaining_cycles = elapsed < timeout_cycles ?
			   timeout_cycles - elapsed : 0;
	first_ns = m1851_boot_timeout_next_interval_ns();

	hrtimer_init(&m1851_boot_timeout_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	m1851_boot_timeout_timer.function = m1851_boot_timeout_expired;
	hrtimer_start(&m1851_boot_timeout_timer, ns_to_ktime(first_ns),
		      HRTIMER_MODE_REL);
	pr_info("120-second restart armed with %llu ms remaining\n",
		div_u64(remaining_cycles * NSEC_PER_SEC,
			m1851_boot_timeout_frequency * NSEC_PER_MSEC));
}

static int __init m1851_early_console_handoff(void)
{
	if (!m1851_early_console_registered)
		return 0;

	unregister_console(&m1851_early_console);
	m1851_early_console_registered = false;
	/* Drain device writes before removing the temporary mapping. */
	mb();
	early_iounmap(m1851_early_ramoops, M1851_EARLY_RAMOOPS_SIZE);
	m1851_early_ramoops = NULL;

	return 0;
}
arch_initcall_sync(m1851_early_console_handoff);

void m1851_boot_timeout_disarm(void)
{
	if (atomic_cmpxchg(&m1851_boot_timeout_state,
			   M1851_BOOT_TIMEOUT_ARMED,
			   M1851_BOOT_TIMEOUT_DISARMING) !=
			   M1851_BOOT_TIMEOUT_ARMED)
		return;

	hrtimer_try_to_cancel(&m1851_boot_timeout_timer);
	m1851_boot_timeout_disable_wdt(m1851_boot_timeout_wdt_base);
	atomic_set(&m1851_boot_timeout_state, M1851_BOOT_TIMEOUT_DISARMED);
	pr_info("disarmed after the first primary display frame\n");
}
EXPORT_SYMBOL_GPL(m1851_boot_timeout_disarm);

bool m1851_boot_timeout_owns_watchdog(void)
{
	return true;
}
