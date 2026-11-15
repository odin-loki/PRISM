/* PRISM device-model harness, Linux flavour: kernel headers and the macro
 * overrides that put the driver's MMIO accessors, logging, bit operations and
 * BUG/WARN under the model (docs/DEVICE_MODEL.md section 3).
 *
 * This is a local stand-in for the kernel-API spec table of requirement R2
 * (another branch): it models only what the harness needs and is the narrow
 * place that table's MMIO spec would replace. The accessors all end in the
 * resolver prism_mmio_read / prism_mmio_write (prism_pre.h).
 *
 * Assumptions this file makes (listed in every report that uses it):
 *  - logging (printk, pr_*, dev_*) has no effect on driver or device state and
 *    its arguments are not evaluated;
 *  - bit operations behave as the C expressions below (non-atomic);
 *  - readX/writeX/ioreadX/iowriteX are ordered and not cached: every call is
 *    one device access of that width.
 */
#ifndef PRISM_LINUX_PRE_H
#define PRISM_LINUX_PRE_H

#include "prism_pre.h"

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/bug.h>

/* ---- MMIO: every accessor is one resolver call ---- */
#undef readb
#undef readw
#undef readl
#undef readq
#undef readb_relaxed
#undef readw_relaxed
#undef readl_relaxed
#undef readq_relaxed
#undef __raw_readb
#undef __raw_readw
#undef __raw_readl
#undef __raw_readq
#undef writeb
#undef writew
#undef writel
#undef writeq
#undef writeb_relaxed
#undef writew_relaxed
#undef writel_relaxed
#undef writeq_relaxed
#undef __raw_writeb
#undef __raw_writew
#undef __raw_writel
#undef __raw_writeq
#undef ioread8
#undef ioread16
#undef ioread32
#undef iowrite8
#undef iowrite16
#undef iowrite32
#define readb(a) ((u8)prism_mmio_read((a), 1))
#define readw(a) ((u16)prism_mmio_read((a), 2))
#define readl(a) ((u32)prism_mmio_read((a), 4))
#define readq(a) ((u64)prism_mmio_read((a), 8))
#define readb_relaxed(a) readb(a)
#define readw_relaxed(a) readw(a)
#define readl_relaxed(a) readl(a)
#define readq_relaxed(a) readq(a)
#define __raw_readb(a) readb(a)
#define __raw_readw(a) readw(a)
#define __raw_readl(a) readl(a)
#define __raw_readq(a) readq(a)
#define writeb(v, a) prism_mmio_write((a), (u64)(v), 1)
#define writew(v, a) prism_mmio_write((a), (u64)(v), 2)
#define writel(v, a) prism_mmio_write((a), (u64)(v), 4)
#define writeq(v, a) prism_mmio_write((a), (u64)(v), 8)
#define writeb_relaxed(v, a) writeb(v, a)
#define writew_relaxed(v, a) writew(v, a)
#define writel_relaxed(v, a) writel(v, a)
#define writeq_relaxed(v, a) writeq(v, a)
#define __raw_writeb(v, a) writeb(v, a)
#define __raw_writew(v, a) writew(v, a)
#define __raw_writel(v, a) writel(v, a)
#define __raw_writeq(v, a) writeq(v, a)
#define ioread8(a) readb(a)
#define ioread16(a) readw(a)
#define ioread32(a) readl(a)
#define iowrite8(v, a) writeb(v, a)
#define iowrite16(v, a) writew(v, a)
#define iowrite32(v, a) writel(v, a)

/* ---- logging: no effect, arguments not evaluated ---- */
#undef printk
#undef pr_emerg
#undef pr_alert
#undef pr_crit
#undef pr_err
#undef pr_warn
#undef pr_notice
#undef pr_info
#undef pr_debug
#undef dev_emerg
#undef dev_alert
#undef dev_crit
#undef dev_err
#undef dev_warn
#undef dev_notice
#undef dev_info
#undef dev_dbg
#undef dev_vdbg
#undef dev_err_probe
#undef dev_warn_once
#undef dev_err_once
#undef dev_info_once
#undef dev_err_ratelimited
#undef dev_warn_ratelimited
#define printk(fmt, ...) ((int)0)
#define pr_emerg(fmt, ...) ((void)0)
#define pr_alert(fmt, ...) ((void)0)
#define pr_crit(fmt, ...) ((void)0)
#define pr_err(fmt, ...) ((void)0)
#define pr_warn(fmt, ...) ((void)0)
#define pr_notice(fmt, ...) ((void)0)
#define pr_info(fmt, ...) ((void)0)
#define pr_debug(fmt, ...) ((void)0)
#define dev_emerg(dev, fmt, ...) ((void)(dev))
#define dev_alert(dev, fmt, ...) ((void)(dev))
#define dev_crit(dev, fmt, ...) ((void)(dev))
#define dev_err(dev, fmt, ...) ((void)(dev))
#define dev_warn(dev, fmt, ...) ((void)(dev))
#define dev_notice(dev, fmt, ...) ((void)(dev))
#define dev_info(dev, fmt, ...) ((void)(dev))
#define dev_dbg(dev, fmt, ...) ((void)(dev))
#define dev_vdbg(dev, fmt, ...) ((void)(dev))
#define dev_warn_once(dev, fmt, ...) ((void)(dev))
#define dev_err_once(dev, fmt, ...) ((void)(dev))
#define dev_info_once(dev, fmt, ...) ((void)(dev))
#define dev_err_ratelimited(dev, fmt, ...) ((void)(dev))
#define dev_warn_ratelimited(dev, fmt, ...) ((void)(dev))
#define dev_err_probe(dev, err, fmt, ...) ((int)(err))

/* ---- BUG / WARN: x86 implements them with inline asm ---- */
#undef BUG
#undef BUG_ON
#undef WARN_ON
#undef WARN_ON_ONCE
#undef WARN
#undef WARN_ONCE
#define BUG() PRISM_MOD_CHECK(0, api, "BUG() reached")
#define BUG_ON(c) PRISM_MOD_CHECK(!(c), api, "BUG_ON condition holds")
#define WARN_ON(c) ((c) != 0)
#define WARN_ON_ONCE(c) ((c) != 0)
#define WARN(c, fmt, ...) ((c) != 0)
#define WARN_ONCE(c, fmt, ...) ((c) != 0)

/* ---- bit operations: x86 implements them with inline asm ---- */
static inline void prism_set_bit(long nr, volatile unsigned long *a) { a[nr / 64] |= 1UL << (nr % 64); }
static inline void prism_clear_bit(long nr, volatile unsigned long *a) { a[nr / 64] &= ~(1UL << (nr % 64)); }
static inline int prism_test_bit(long nr, const volatile unsigned long *a) { return (a[nr / 64] >> (nr % 64)) & 1; }
#undef set_bit
#undef clear_bit
#undef __set_bit
#undef __clear_bit
#undef test_bit
#define set_bit(nr, a) prism_set_bit((nr), (a))
#define clear_bit(nr, a) prism_clear_bit((nr), (a))
#define __set_bit(nr, a) prism_set_bit((nr), (a))
#define __clear_bit(nr, a) prism_clear_bit((nr), (a))
#define test_bit(nr, a) prism_test_bit((nr), (a))

/* ---- allocation: the real kmalloc family is size-class machinery ---- */
static void *prism_g_alloc(unsigned long size, int zero);
#undef kmalloc
#undef kzalloc
#undef kcalloc
#undef kmalloc_array
#undef kfree
#undef kvfree
#undef kfree_sensitive
#define kmalloc(size, flags) prism_kmalloc((size), 0)
#define kzalloc(size, flags) prism_kmalloc((size), 1)
#define kcalloc(n, size, flags) prism_kmalloc((unsigned long)(n) * (size), 1)
#define kmalloc_array(n, size, flags) prism_kmalloc((unsigned long)(n) * (size), 0)
#define kfree(p) prism_kfree(p)
#define kvfree(p) prism_kfree(p)
#define kfree_sensitive(p) prism_kfree(p)
static void *prism_kmalloc(unsigned long size, int zero);
static void prism_kfree(const void *p);

/* ---- locks ---- */
static void prism_g_lock(const void *l, int sleeps, int irqoff);
static void prism_g_unlock(const void *l, int irqon);
#undef mutex_init
#undef mutex_lock
#undef mutex_unlock
#undef spin_lock_init
#undef spin_lock
#undef spin_unlock
#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#undef spin_lock_irq
#undef spin_unlock_irq
#define mutex_init(m) ((void)(m))
#define mutex_lock(m) prism_g_lock((m), 1, 0)
#define mutex_unlock(m) prism_g_unlock((m), 0)
#define spin_lock_init(l) ((void)(l))
#define spin_lock(l) prism_g_lock((l), 0, 0)
#define spin_unlock(l) prism_g_unlock((l), 0)
#define spin_lock_irq(l) prism_g_lock((l), 0, 1)
#define spin_unlock_irq(l) prism_g_unlock((l), 1)
#define spin_lock_irqsave(l, f) do { (void)(f); prism_g_lock((l), 0, 1); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); prism_g_unlock((l), 1); } while (0)

/* device-managed wakeup: no state the harness models */
#undef devm_device_init_wakeup
#undef devm_pm_set_wake_irq
#define devm_device_init_wakeup(dev) ((void)(dev), 0)
#define devm_pm_set_wake_irq(dev, irq) ((void)(dev), (void)(irq), 0)


/* ---- error pointers: ERR_PTR / IS_ERR / PTR_ERR ----
 * The kernel encodes -errno as a pointer value in the top page. PIR does not
 * model address values (UNENCODED: ptrtoint), so error pointers are one
 * marker object: ERR_PTR(e) is the marker, IS_ERR(p) is p == marker, and
 * PTR_ERR(p) is an arbitrary errno -4095..-1. This over-approximates (the
 * code is forgotten) and never hides a path; a driver that branches on a
 * specific errno sees all outcomes. */
static char prism_err_marker[1];
#undef ERR_PTR
#undef IS_ERR
#undef IS_ERR_OR_NULL
#undef PTR_ERR
#undef PTR_ERR_OR_ZERO
#undef ERR_CAST
#define ERR_PTR(e) ((void)(e), (void *)prism_err_marker)
#define IS_ERR(p) ((const void *)(p) == (const void *)prism_err_marker)
#define IS_ERR_OR_NULL(p) ((const void *)(p) == (const void *)0 || IS_ERR(p))
#define PTR_ERR(p) ((void)(p), (long)(-1 - (long)prism_nondet_choice(4095)))
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p) ? PTR_ERR(p) : 0)
#define ERR_CAST(p) ((void *)(p))

#endif
