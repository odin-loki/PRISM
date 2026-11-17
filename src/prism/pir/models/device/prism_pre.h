/* PRISM device-model harness: preamble (docs/DEVICE_MODEL.md section 3).
 *
 * Included first by every generated harness unit. No kernel headers here:
 * only the checking primitives the harness and the stubs share. The
 * __prism_check / __VERIFIER_* names are PIR intrinsics (translate.cpp):
 * __prism_check(ok, class, message) is a property whose violation is the
 * finding; __VERIFIER_nondet_* are arbitrary values.
 */
#ifndef PRISM_PRE_H
#define PRISM_PRE_H

void __prism_check(int ok, const char *cls, const char *msg);
void __VERIFIER_assume(int cond);
int __VERIFIER_nondet_int(void);
unsigned long __VERIFIER_nondet_ulong(void);

/* A module property. `prop` is one of: pairing, lock, use_after_remove,
 * protocol, api. It names the property in the finding message, which is how
 * prism-module reads results back per property. The generator defines
 * PRISM_OFF_<prop> for the properties a run does not select (the report
 * says so). */
#define PRISM_MOD_CHECK(c, prop, text) PRISM_CK_##prop((c), "[module:" #prop "] " text)
#ifdef PRISM_OFF_pairing
#define PRISM_CK_pairing(c, m) ((void)0)
#else
#define PRISM_CK_pairing(c, m) __prism_check(((c) != 0), "FUNC-CONTRACT", m)
#endif
#ifdef PRISM_OFF_lock
#define PRISM_CK_lock(c, m) ((void)0)
#else
#define PRISM_CK_lock(c, m) __prism_check(((c) != 0), "FUNC-CONTRACT", m)
#endif
#ifdef PRISM_OFF_use_after_remove
#define PRISM_CK_use_after_remove(c, m) ((void)0)
#else
#define PRISM_CK_use_after_remove(c, m) __prism_check(((c) != 0), "FUNC-CONTRACT", m)
#endif
#ifdef PRISM_OFF_protocol
#define PRISM_CK_protocol(c, m) ((void)0)
#else
#define PRISM_CK_protocol(c, m) __prism_check(((c) != 0), "FUNC-CONTRACT", m)
#endif
#ifdef PRISM_OFF_api
#define PRISM_CK_api(c, m) ((void)0)
#else
#define PRISM_CK_api(c, m) __prism_check(((c) != 0), "FUNC-CONTRACT", m)
#endif

/* Coverage points (rows "cover-*"): a row compiled with -DPRISM_COVER=<id> reports FAILED exactly when the
 * point <id> is reachable. A coverage row that PIR PROVES means the point is unreachable: the other rows'
 * verdicts are then vacuous for that part of the module, and the report says so. */
#ifdef PRISM_COVER
#define PRISM_COVER_POINT(id) do { if ((id) == PRISM_COVER) __prism_check(0, "FUNC-CONTRACT", "[module:cover] point reached"); } while (0)
#else
#define PRISM_COVER_POINT(id) ((void)0)
#endif

static inline unsigned long long prism_nondet_u64(void) { return (unsigned long long)__VERIFIER_nondet_ulong(); }
static inline unsigned prism_nondet_choice(unsigned n)
{
	unsigned c = (unsigned)__VERIFIER_nondet_int();
	__VERIFIER_assume(c < n);
	return c;
}
static inline int prism_nondet_bool(void) { return (int)prism_nondet_choice(2); }
static inline int prism_nondet_int(void) { return __VERIFIER_nondet_int(); }

/* The resolver interface: the one hook between MMIO accessors and a device
 * model. Every readl, writel, ioreadN, iowriteN, bus_space access of the driver
 * becomes one of these two calls (address and width in bytes); they resolve
 * to the byte offset in the device window and then to prism_dev_read /
 * prism_dev_write, which a generated device model defines (or the default:
 * an unconstrained read of the right width, writes ignored). */
unsigned long long prism_mmio_read(const volatile void *addr, unsigned width);
void prism_mmio_write(volatile void *addr, unsigned long long val, unsigned width);
unsigned long long prism_dev_read(unsigned long off, unsigned width);
void prism_dev_write(unsigned long off, unsigned long long val, unsigned width);

#endif
