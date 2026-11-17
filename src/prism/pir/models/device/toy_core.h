/* PRISM device-model harness, "toy" flavour: definitions (after the driver,
 * the device model and prism_ghost.h). */
#ifndef PRISM_TOY_CORE_H
#define PRISM_TOY_CORE_H

void *kmalloc(size_t size)
{
	void *p = prism_g_alloc(size, 0);
	if (p)
		prism_heap++;
	return p;
}

void *kzalloc(size_t size)
{
	void *p = prism_g_alloc(size, 1);
	if (p)
		prism_heap++;
	return p;
}

void kfree(const void *p)
{
	if (p) {
		PRISM_MOD_CHECK(prism_heap > 0, pairing, "kfree of memory that kmalloc did not return");
		if (prism_heap > 0)
			prism_heap--;
		__prism_free((void *)p, 2);
	}
}

void *devm_kzalloc(struct device *dev, size_t size)
{
	void *p = prism_g_alloc(size, 1);
	(void)dev;
	if (p)
		prism_dr_add(PRISM_DR_FREE, p);
	return p;
}

void __iomem *ioremap(unsigned long phys, unsigned long size)
{
	(void)phys;
	return (void __iomem *)prism_g_map(size);
}

void iounmap(void __iomem *addr)
{
	(void)addr;
	prism_g_unmap();
}

int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags, const char *name, void *dev)
{
	int r = prism_g_request_irq((int)irq, (int (*)(int, void *))h, flags, dev);
	(void)name;
	if (r == 0)
		prism_irq_spurious = PRISM_IRQ_SPURIOUS == 1 || (PRISM_IRQ_SPURIOUS == 2 && (flags & IRQF_SHARED) != 0);
	return r;
}

void free_irq(unsigned int irq, void *dev_id)
{
	(void)prism_g_free_irq((int)irq, dev_id);
}

int toy_request_region(unsigned long start, unsigned long len)
{
	(void)start; (void)len;
	if (prism_nondet_bool())
		return -16;
	PRISM_MOD_CHECK(prism_regions == 0, pairing, "region requested twice without release");
	prism_regions++;
	return 0;
}

void toy_release_region(unsigned long start, unsigned long len)
{
	(void)start; (void)len;
	PRISM_API_POINT();
	PRISM_MOD_CHECK(prism_regions > 0, pairing, "release of a region that is not held");
	if (prism_regions > 0)
		prism_regions--;
}

void mutex_lock(struct mutex *m) { prism_g_lock(m, 1, 0); }
void mutex_unlock(struct mutex *m) { prism_g_unlock(m, 0); }

#endif
