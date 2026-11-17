// Generated register-trace guest for the QEMU 'virt' PL031 (0x09010000); prints one line per read.
// Output: "R <off-hex> <value-hex>" per read, "W <off-hex> <value-hex>" per write, "T <dr>" after a wait.
typedef unsigned int u32;
typedef unsigned long long u64;
#define UART ((volatile u32 *)0x09000000UL)
#define RTC  ((volatile u32 *)0x09010000UL)
static void putc_(char c) { *UART = (u32)c; }
static void qemu_exit(void) { static u64 blk[2] = {0x20026, 0}; register u64 x0 __asm__("x0") = 0x18; register u64 *x1 __asm__("x1") = blk; __asm__ volatile("hlt #0xf000" :: "r"(x0), "r"(x1) : "memory"); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(u32 v) { const char *h = "0123456789abcdef"; int started = 0; for (int i = 28; i >= 0; i -= 4) { int d = (v >> i) & 15; if (d || started || i == 0) { putc_(h[d]); started = 1; } } }
static u32 rd(u32 off) { u32 v = RTC[off / 4]; puts_("R "); puthex(off); putc_(' '); puthex(v); putc_('\n'); return v; }
static void wr(u32 off, u32 v) { RTC[off / 4] = v; puts_("W "); puthex(off); putc_(' '); puthex(v); putc_('\n'); }
// wait until DR has advanced by n seconds (at most ~n+2 s of wall time), printing the DR seen
static void wait_secs(u32 n) { u32 d0 = RTC[0]; u32 spin = 0; while ((u32)(RTC[0] - d0) < n && spin < 400000000u) spin++; puts_("T "); puthex(RTC[0]); putc_('\n'); }
void guest_main(void)
{
#include "TRACE"
    puts_("DONE\n");
    qemu_exit();
}
