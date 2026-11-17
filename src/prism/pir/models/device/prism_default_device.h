/* Default device: no model. Every read is an unconstrained value of the
 * right width, writes have no effect, the interrupt line may be asserted at
 * any time. Results that use it name "no device model (unconstrained)" in
 * their assumption list. PRISM_DEV_WINDOW is set by the harness generator. */
#define PRISM_DEV_REQUIRE(c, text) PRISM_MOD_CHECK(c, protocol, "default: " text)
static void prism_dev_init(void) {}
unsigned long long prism_dev_read(unsigned long off, unsigned width)
{
	(void)off;
	return width >= 8 ? prism_nondet_u64() : (prism_nondet_u64() & ((1ULL << (width * 8)) - 1));
}
void prism_dev_write(unsigned long off, unsigned long long val, unsigned width)
{
	(void)off; (void)val; (void)width;
}
int prism_dev_irq_level(unsigned line) { (void)line; return prism_nondet_bool(); }
#define PRISM_DEV_IRQ_LINES 1
#define PRISM_DEV_EVENTS 0
static void prism_dev_event(void) {}
