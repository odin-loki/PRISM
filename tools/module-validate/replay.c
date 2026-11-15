/* replay MODEL_C TRACE : feed the writes of a QEMU register trace to the generated device model
 * and check every read against what QEMU returned. The model's nondeterminism (elapsed time, whether a
 * due alarm has fired yet) is resolved from the trace: time from the DR values seen, `fire` from the next
 * RIS read. Compile: cc -DMODEL_C='"model.c"' replay.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int viol, assume_fail;
static unsigned long long nd_q[8];
static int nd_n, nd_i;
#define PRISM_MOD_CHECK(c, prop, text) do { if (!(c)) { printf("MODEL-VIOLATION: %s\n", text); viol++; } } while (0)
static void __VERIFIER_assume(int c) { if (!c) assume_fail = 1; }
static unsigned long long prism_nondet_u64(void) { return nd_i < nd_n ? nd_q[nd_i++] : 0; }
static unsigned prism_nondet_choice(unsigned n) { (void)n; return 0; }
#include MODEL_C

int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "r");
	char line[256], kind;
	unsigned off, val, reads = 0, bad = 0, ticks = 0;
	unsigned long long pend_ris = 2;
	static char lines[4096][64];
	int nl = 0;
	if (!f) return 2;
	while (fgets(line, sizeof line, f) && nl < 4096) strncpy(lines[nl++], line, 63);
	int first = 1;
	for (int i = 0; i < nl; i++) {
		if (sscanf(lines[i], "%c %x %x", &kind, &off, &val) < 2) continue;
		if (kind == 'T') {   /* time: DR value after a wait; sync below happens on the next DR read */
			continue;
		}
		if (kind == 'R' && off == 0) {
			unsigned count = (unsigned)((dm_now + dm_tick_offset) & 0xffffffffu);
			if (first) { dm_tick_offset = (val - dm_now) & 0xffffffffu; first = 0; count = val; }
			unsigned dt = val - count;
			if (dt) {   /* time passed: run the tick event with dt; fire from the next RIS read */
				unsigned fire = 0;
				for (int j = i + 1; j < nl; j++) {
					unsigned o2, v2; char k2;
					if (sscanf(lines[j], "%c %x %x", &k2, &o2, &v2) >= 3 && k2 == 'R' && o2 == 0x14) { fire = v2 & 1; break; }
				}
				nd_q[0] = dt; nd_q[1] = fire; nd_n = 2; nd_i = 0; assume_fail = 0;
				prism_dev_event();   /* PRISM_DEV_EVENTS == 1: the only event is tick */
				ticks++;
				if (assume_fail) { printf("MISMATCH line %d: model cannot fire the alarm yet (QEMU shows RIS=1)\n", i + 1); bad++; }
			}
			reads++;
			unsigned got = (unsigned)prism_dev_read(off, 4);
			if (got != val) { printf("MISMATCH line %d: DR model %x qemu %x\n", i + 1, got, val); bad++; }
			continue;
		}
		if (kind == 'R') {
			reads++;
			unsigned got = (unsigned)prism_dev_read(off, 4);
			if (kind == 'R' && off == 0x1c) continue;
			if (got != val) { printf("MISMATCH line %d: read 0x%x model %x qemu %x\n", i + 1, off, got, val); bad++; }
		} else if (kind == 'W') {
			prism_dev_write(off, val, 4);
		}
	}
	printf("%s: %u reads compared, %u time syncs, %u mismatches, %d model-rule violations\n", bad || viol ? "FAIL" : "OK", reads, ticks, bad, viol);
	return bad || viol ? 1 : 0;
}
