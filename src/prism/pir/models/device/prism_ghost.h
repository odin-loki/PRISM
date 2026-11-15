/* PRISM device-model harness: ghost state and generic kernel-object
 * operations (docs/DEVICE_MODEL.md sections 3 and 5). Included after the
 * device model's C, before the kernel-flavour stubs. Every operation here is
 * ordinary C that PIR checks; the properties are PRISM_MOD_CHECKs.
 *
 * What is modelled (and nothing else): the MMIO window of one device, the
 * IRQ registration of one line, bus regions, device-managed (devm) resources
 * released after remove or a failed probe in reverse order, plain heap
 * allocations, a small lock table, interrupt/event points. */
#ifndef PRISM_GHOST_H
#define PRISM_GHOST_H

/* PIR intrinsics (translate.cpp model_intrinsic): a heap object (kind 2) with uninitialised (0), zero (1) or arbitrary (2) bytes */
void *__prism_alloc(unsigned long size, int kind, int init);
void __prism_free(void *p, int kind);

#ifndef PRISM_IRQ_BUDGET
#define PRISM_IRQ_BUDGET 2
#endif
#ifndef PRISM_EVENT_BUDGET
#define PRISM_EVENT_BUDGET 2
#endif

static char prism_mmio_window[PRISM_DEV_WINDOW];
/* the base is read through a variable: PIR models pointer differences of two loaded pointers, not of a constant address */
static char *prism_mmio_base = prism_mmio_window;

enum { PRISM_PH_INIT, PRISM_PH_PROBING, PRISM_PH_BOUND, PRISM_PH_REMOVING, PRISM_PH_REMOVED, PRISM_PH_FAILED };
static int prism_phase;
static int prism_in_irq;
static int prism_irqs_off;
static int prism_irq_budget = PRISM_IRQ_BUDGET;
static int prism_event_budget = PRISM_EVENT_BUDGET;
static int prism_irq_spurious;          /* handler may run with the line deasserted (shared line) */

static int prism_regions;               /* bus regions held */
static int prism_heap;                  /* kmalloc'd, not freed (non-devm) */
static int prism_maps;                  /* ioremap'd windows not unmapped */
static int prism_mapped;                /* the window is mapped: the device may be accessed */
static int prism_clk;                   /* clock enable count */

static int prism_irq_on;
static int prism_irq_no;
static unsigned long prism_irq_flags;
static void *prism_irq_cookie;
static int (*prism_irq_fn)(int, void *);

#define PRISM_DEVRES_MAX 6
enum { PRISM_DR_FREE = 1, PRISM_DR_UNMAP, PRISM_DR_IRQ, PRISM_DR_CLEAR };
static int prism_dr_kind[PRISM_DEVRES_MAX];
static void *prism_dr_ptr[PRISM_DEVRES_MAX];
static int prism_dr_n;

#define PRISM_LOCKS 2
static const void *prism_lock_ptr[PRISM_LOCKS];
static int prism_lock_held[PRISM_LOCKS];
static int prism_lock_used;

/* ---- device events and interrupts ---- */

static void prism_irq_deliver(void)
{
	if (prism_in_irq || prism_irqs_off || !prism_irq_on || prism_irq_budget <= 0)
		return;
	if (!(prism_dev_irq_level(0) || prism_irq_spurious))
		return;
	prism_irq_budget--;
	PRISM_COVER_POINT(3);                /* the handler runs */
	prism_in_irq = 1;
	(void)prism_irq_fn(prism_irq_no, prism_irq_cookie);
	prism_in_irq = 0;
}

/* The device may do something (time passes, a transfer completes) and then
 * its interrupt line may be serviced. Called at the points the module
 * description selects. */
static void prism_async_point(void)
{
	if (prism_in_irq)
		return;
	if (PRISM_DEV_EVENTS && prism_event_budget > 0 && prism_nondet_bool()) {
		prism_event_budget--;
		prism_dev_event();
	}
	if (prism_nondet_bool())
		prism_irq_deliver();
}

/* ---- MMIO ---- */

unsigned long long prism_mmio_read(const volatile void *addr, unsigned width)
{
	long off = (const volatile char *)addr - (const volatile char *)prism_mmio_base;
	PRISM_MOD_CHECK(prism_mapped, use_after_remove, "MMIO read while the device window is not mapped");
	PRISM_MOD_CHECK(off >= 0 && (unsigned long)off + width <= PRISM_DEV_WINDOW, protocol, "MMIO read outside the device window");
#ifdef PRISM_IRQ_AT_MMIO
	prism_async_point();
#endif
	return prism_dev_read((unsigned long)off, width);
}

void prism_mmio_write(volatile void *addr, unsigned long long val, unsigned width)
{
	long off = (const volatile char *)addr - (const volatile char *)prism_mmio_base;
	PRISM_MOD_CHECK(prism_mapped, use_after_remove, "MMIO write while the device window is not mapped");
	PRISM_MOD_CHECK(off >= 0 && (unsigned long)off + width <= PRISM_DEV_WINDOW, protocol, "MMIO write outside the device window");
	prism_dev_write((unsigned long)off, val, width);
#ifdef PRISM_IRQ_AT_MMIO
	prism_async_point();
#endif
}

/* an API-call point: interrupts and device events may happen here */
#ifdef PRISM_IRQ_AT_API
#define PRISM_API_POINT() prism_async_point()
#else
#define PRISM_API_POINT() ((void)0)
#endif

/* ---- resources ---- */

static void *prism_g_map(unsigned long size)
{
	if (prism_nondet_bool())
		return (void *)0;
	PRISM_MOD_CHECK(size <= PRISM_DEV_WINDOW, protocol, "mapping larger than the device window");
	prism_maps++;
	prism_mapped = 1;
	return (void *)prism_mmio_window;
}

static void prism_g_unmap_core(void)
{
	PRISM_MOD_CHECK(prism_maps > 0, pairing, "unmap of a window that is not mapped");
	if (prism_maps > 0)
		prism_maps--;
	if (prism_maps == 0)
		prism_mapped = 0;
}

static void prism_g_unmap(void)
{
	PRISM_API_POINT();
	prism_g_unmap_core();
}

static void *prism_g_alloc(unsigned long size, int zero)
{
	if (prism_nondet_bool())
		return (void *)0;                               /* allocation may fail */
	if (zero)
		return __prism_alloc(size, 2, 1);
	return __prism_alloc(size, 2, 0);
}

static void prism_dr_add(int kind, void *p)
{
	__VERIFIER_assume(prism_dr_n < PRISM_DEVRES_MAX);   /* bound: at most 6 devm resources */
	prism_dr_kind[prism_dr_n] = kind;
	prism_dr_ptr[prism_dr_n] = p;
	prism_dr_n++;
}

static int prism_g_request_irq(int irq, int (*fn)(int, void *), unsigned long flags, void *cookie)
{
	PRISM_MOD_CHECK(fn != (int (*)(int, void *))0, api, "request_irq with a NULL handler");
	if (prism_nondet_bool())
		return -16;                                     /* -EBUSY: the failure path */
	PRISM_MOD_CHECK(!prism_irq_on, pairing, "irq requested twice without free_irq");
	prism_irq_on = 1;
	prism_irq_no = irq;
	prism_irq_flags = flags;
	prism_irq_fn = fn;
	prism_irq_cookie = cookie;
	PRISM_API_POINT();                                  /* the handler may fire as soon as request_irq returns */
	return 0;
}

static void *prism_g_free_irq(int irq, void *cookie)
{
	PRISM_API_POINT();
	PRISM_MOD_CHECK(prism_irq_on, pairing, "free_irq of an irq that is not requested");
	PRISM_MOD_CHECK(irq == prism_irq_no, pairing, "free_irq with a different irq number than request_irq");
	PRISM_MOD_CHECK(cookie == prism_irq_cookie, pairing, "free_irq with a different dev_id than request_irq");
	prism_irq_on = 0;                                   /* free_irq waits for a running handler: none runs afterwards */
	return prism_irq_cookie;
}

/* devres: released in reverse order when a failed probe returns and after remove returns */
static void prism_devres_release(void)
{
	int i;
	for (i = PRISM_DEVRES_MAX - 1; i >= 0; i--) {
		if (i >= prism_dr_n)
			continue;
		if (prism_dr_kind[i] == PRISM_DR_FREE)
			__prism_free(prism_dr_ptr[i], 2);
		else if (prism_dr_kind[i] == PRISM_DR_UNMAP)
			prism_g_unmap_core();
		else if (prism_dr_kind[i] == PRISM_DR_CLEAR)
			*(int *)prism_dr_ptr[i] = 0;
		else if (prism_dr_kind[i] == PRISM_DR_IRQ)
			prism_irq_on = 0;                       /* devm_free_irq: no point, no checks: it is the framework's own release */
	}
	prism_dr_n = 0;
}

/* ---- locks (process-context mutex or spinlock; the same table) ---- */

static int prism_lock_slot(const void *l)
{
	if (prism_lock_used > 0 && prism_lock_ptr[0] == l)
		return 0;
	if (prism_lock_used > 1 && prism_lock_ptr[1] == l)
		return 1;
	__VERIFIER_assume(prism_lock_used < PRISM_LOCKS);   /* bound: at most 2 distinct locks */
	prism_lock_ptr[prism_lock_used] = l;
	prism_lock_held[prism_lock_used] = 0;
	return prism_lock_used++;
}

static void prism_g_lock(const void *l, int sleeps, int irqoff)
{
	int s = prism_lock_slot(l);
	PRISM_MOD_CHECK(!(sleeps && prism_in_irq), lock, "sleeping lock taken in interrupt context");
	PRISM_MOD_CHECK(!prism_lock_held[s], lock, "lock taken while already held (self-deadlock)");
	prism_lock_held[s] = 1;
	if (irqoff)
		prism_irqs_off++;
	PRISM_API_POINT();
}

static void prism_g_unlock(const void *l, int irqon)
{
	int s;
	PRISM_API_POINT();
	s = prism_lock_slot(l);
	PRISM_MOD_CHECK(prism_lock_held[s], lock, "unlock of a lock that is not held");
	prism_lock_held[s] = 0;
	if (irqon && prism_irqs_off > 0)
		prism_irqs_off--;
}

static int prism_locks_held(void)
{
	return (prism_lock_used > 0 && prism_lock_held[0]) + (prism_lock_used > 1 && prism_lock_held[1]);
}

/* ---- checkpoints ---- */

#define PRISM_CHECKPOINT(tag)                                                                                   \
	do {                                                                                                    \
		PRISM_MOD_CHECK(prism_regions == 0, pairing, tag ": a bus region is still held (no release on this path)"); \
		PRISM_MOD_CHECK(!prism_irq_on, pairing, tag ": the irq is still requested (no free_irq on this path)");      \
		PRISM_MOD_CHECK(prism_heap == 0, pairing, tag ": a plain allocation is still live (no free on this path)"); \
		PRISM_MOD_CHECK(prism_maps == 0, pairing, tag ": an ioremap is still mapped (no unmap on this path)");       \
		PRISM_MOD_CHECK(prism_clk == 0, pairing, tag ": a clock enable is still outstanding");                     \
		PRISM_MOD_CHECK(prism_locks_held() == 0, lock, tag ": a lock is still held");                          \
	} while (0)

#define PRISM_LOCKCHECK(tag) PRISM_MOD_CHECK(prism_locks_held() == 0, lock, tag ": returned with a lock held")

#endif
