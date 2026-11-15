/* PRISM device-model harness, Linux flavour: definitions of the kernel
 * functions the headers only declare (after the driver, the device model and
 * prism_ghost.h). Each one is the generic operation of prism_ghost.h under
 * its kernel name and prototype. A local stand-in for R2's kernel API table.
 *
 * Modelled failures: every allocation, mapping, region request and irq
 * request may fail (that is how failing-probe paths are reached). Threaded
 * irq handlers are modelled by their hard-irq function; the thread function
 * is not run (assumption).
 */
#ifndef PRISM_LINUX_CORE_H
#define PRISM_LINUX_CORE_H

static void *prism_kmalloc(unsigned long size, int zero)
{
	void *p = prism_g_alloc(size, zero);
	if (p)
		prism_heap++;
	return p;
}

static void prism_kfree(const void *p)
{
	if (p) {
		PRISM_MOD_CHECK(prism_heap > 0, pairing, "kfree of memory that kmalloc did not return");
		if (prism_heap > 0)
			prism_heap--;
		__prism_free((void *)p, 2);
	}
}

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	void *p = prism_g_alloc(size, (gfp & __GFP_ZERO) != 0);
	(void)dev;
	if (p)
		prism_dr_add(PRISM_DR_FREE, p);
	return p;
}

void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
	void *p = devm_kmalloc(dev, len, gfp);
	if (p)
		__builtin_memcpy(p, src, len);
	return p;
}

void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size)
{
	void *p = prism_g_map(size);
	(void)dev; (void)offset;
	if (p)
		prism_dr_add(PRISM_DR_UNMAP, p);
	return (void __iomem *)p;
}

void __iomem *ioremap(resource_size_t offset, unsigned long size)
{
	(void)offset;
	return (void __iomem *)prism_g_map(size);
}

void iounmap(volatile void __iomem *addr)
{
	(void)addr;
	prism_g_unmap();
}

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
			 const char *name, void *dev)
{
	int r = prism_g_request_irq((int)irq, (int (*)(int, void *))handler, flags, dev);
	(void)thread_fn; (void)name;
	if (r == 0)
		prism_irq_spurious = PRISM_IRQ_SPURIOUS == 1 || (PRISM_IRQ_SPURIOUS == 2 && (flags & IRQF_SHARED) != 0);
	return r;
}

const void *free_irq(unsigned int irq, void *dev_id)
{
	return prism_g_free_irq((int)irq, dev_id);
}

int devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long irqflags, const char *devname, void *dev_id)
{
	int r = request_threaded_irq(irq, handler, thread_fn, irqflags, devname, dev_id);
	(void)dev;
	if (r == 0)
		prism_dr_add(PRISM_DR_IRQ, (void *)0);
	return r;
}

void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
	(void)dev;
	(void)prism_g_free_irq((int)irq, dev_id);
}

#endif
