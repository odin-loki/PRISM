/* PRISM device-model harness, "toy" flavour: a self-contained mini kernel API
 * for fixtures and the documentation example. No kernel headers; the names
 * mirror Linux so a toy driver reads like a kernel driver. Same generic
 * operations as the Linux flavour (prism_ghost.h). */
#ifndef PRISM_TOY_PRE_H
#define PRISM_TOY_PRE_H

#include "prism_pre.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long size_t;

struct device { void *driver_data; };
typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQF_SHARED 0x80UL
typedef irqreturn_t (*irq_handler_t)(int, void *);
struct mutex { int dummy; };

#define __iomem
#define NULL ((void *)0)

#define readb(a) ((u8)prism_mmio_read((a), 1))
#define readw(a) ((u16)prism_mmio_read((a), 2))
#define readl(a) ((u32)prism_mmio_read((a), 4))
#define writeb(v, a) prism_mmio_write((a), (u64)(v), 1)
#define writew(v, a) prism_mmio_write((a), (u64)(v), 2)
#define writel(v, a) prism_mmio_write((a), (u64)(v), 4)

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(const void *p);
void *devm_kzalloc(struct device *dev, size_t size);
void __iomem *ioremap(unsigned long phys, unsigned long size);
void iounmap(void __iomem *addr);
int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags, const char *name, void *dev);
void free_irq(unsigned int irq, void *dev_id);
int toy_request_region(unsigned long start, unsigned long len);
void toy_release_region(unsigned long start, unsigned long len);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

#endif
