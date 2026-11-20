/* A toy driver for the fixtures: probe/remove, an interrupt handler, three operations.
 * Variants are selected with `define` in the module description (or --define):
 *   LEAK        the request_irq failure path forgets iounmap       (resource pairing)
 *   UAF         remove frees the state before free_irq             (use after remove, native UAF)
 *   LOCKLEAK    start() returns with the lock held on a busy device (lock discipline)
 *   SHORT_TABLE the channel table has 2 entries: OOB for channel 2  (memory error, even with the model)
 *   NOCHECK     data() writes without checking busy                 (device protocol)

 * read unconstrained) the channel field of the status register can be 3 and index past the 3-entry
 * table: a hostile-device memory error.
 */
#ifdef SHORT_TABLE
#define TABLE_ENTRIES 2
#else
#define TABLE_ENTRIES 3
#endif

struct tdev {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	unsigned count;
	int *table;
};

static irqreturn_t toy_irq(int irq, void *id)
{
	struct tdev *t = id;
	u32 st = readl(t->base + 0x08);

	if (!(st & 1))
		return IRQ_NONE;
	writel(1, t->base + 0x08);   /* write 1 to clear */
	t->count++;
	return IRQ_HANDLED;
}

static int toy_probe(struct device *dev)
{
	struct tdev *t;
	int ret, i;

	ret = toy_request_region(0x1000, 0x100);
	if (ret)
		return ret;
	t = kzalloc(sizeof(*t));
	if (!t) {
		ret = -12;
		goto err_region;
	}
	t->dev = dev;
	t->table = kzalloc(sizeof(int) * TABLE_ENTRIES);
	if (!t->table) {
		ret = -12;
		goto err_free;
	}
	for (i = 0; i < TABLE_ENTRIES; i++)
		t->table[i] = 10 * (i + 1);
	t->base = ioremap(0x1000, 0x100);
	if (!t->base) {
		ret = -12;
		goto err_table;
	}
	dev->driver_data = t;
	ret = request_irq(5, toy_irq, 0, "toy", t);
	if (ret)
		goto err_unmap;
	writel(1, t->base + 0x0c);   /* enable the done interrupt */
	return 0;

err_unmap:
#ifndef LEAK
	iounmap(t->base);
#endif
err_table:
	kfree(t->table);
err_free:
	kfree(t);
err_region:
	toy_release_region(0x1000, 0x100);
	return ret;
}

static void toy_remove(struct device *dev)
{
	struct tdev *t = dev->driver_data;
	void __iomem *base = t->base;
	int *table = t->table;

#ifdef UAF
	kfree(t);
	free_irq(5, t);          /* the handler can still run here, on freed state */
	iounmap(base);
	kfree(table);
#else
	writel(0, base + 0x0c);  /* disable the interrupt, then stop the handler, then free its state */
	free_irq(5, t);
	iounmap(base);
	kfree(table);
	kfree(t);
#endif
	toy_release_region(0x1000, 0x100);
}

static int toy_start(struct device *dev)
{
	struct tdev *t = dev->driver_data;

	mutex_lock(&t->lock);
	if (readl(t->base + 0x04) & 1) {
#ifndef LOCKLEAK
		mutex_unlock(&t->lock);
#endif
		return -16;
	}
	writel(1, t->base + 0x00);
	mutex_unlock(&t->lock);
	return 0;
}

static int toy_chan(struct device *dev)
{
	struct tdev *t = dev->driver_data;
	u32 st = readl(t->base + 0x04);

	return t->table[(st >> 8) & 3];
}

static void toy_data(struct device *dev)
{
	struct tdev *t = dev->driver_data;

#ifndef NOCHECK
	if (readl(t->base + 0x04) & 1)
		return;
#endif
	writel(0x55, t->base + 0x10);
}
