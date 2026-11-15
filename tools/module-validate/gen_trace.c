/* gen_trace SEED NOPS > trace.inc : a random register trace for guest_template.c.
 * Time passes only through wait_secs (at most 3 per trace). Before every read of a status register the
 * trace reads DR, so the replay can synchronise the model's clock to the one QEMU shows. */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long s;
static unsigned rnd(void) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(s >> 33); }

int main(int argc, char **argv)
{
	int seed = argc > 1 ? atoi(argv[1]) : 1, n = argc > 2 ? atoi(argv[2]) : 20, waits = 0;
	s = (unsigned long long)seed * 2654435761ULL + 12345;
	printf("\tu32 dr = rd(0x00);\n");
	if (seed >= 100) {   /* alarm-focused: arm, wait, observe, clear; three rounds, so three waits */
		for (int b = 0; b < 3; b++) {
			unsigned k = rnd() % 3, w = 1 + rnd() % 2;
			printf("\tdr = rd(0x00);\n\twr(0x10, 1);\n\twr(0x04, dr + %u);\n\trd(0x14);\n\trd(0x18);\n", k);
			printf("\twait_secs(%u);\n\tdr = rd(0x00);\n\trd(0x14);\n\trd(0x18);\n", w);
			printf("\twr(0x1c, %u);\n\trd(0x14);\n\trd(0x18);\n", rnd() % 2);
		}
		return 0;
	}
	for (int i = 0; i < n; i++) {
		switch (rnd() % 14) {
		case 0: printf("\tdr = rd(0x00);\n"); break;
		case 1: printf("\trd(0x04);\n"); break;
		case 2: printf("\trd(0x08);\n"); break;
		case 3: printf("\trd(0x0c);\n"); break;
		case 4: printf("\trd(0x10);\n"); break;
		case 5: printf("\tdr = rd(0x00);\n\trd(0x14);\n\trd(0x18);\n"); break;
		case 6: printf("\trd(0x%x);\n", 0xfe0 + 4 * (rnd() % 8)); break;
		case 7: printf("\tdr = rd(0x00);\n\twr(0x04, dr + %u);\n\tdr = rd(0x00);\n\trd(0x14);\n", rnd() % 4); break;
		case 8: printf("\twr(0x08, 0x%x);\n\tdr = rd(0x00);\n\trd(0x08);\n", rnd() % 3 ? 1000000u + rnd() % 1000 : rnd()); break;
		case 9: printf("\twr(0x10, %u);\n\trd(0x10);\n\trd(0x18);\n", rnd() % 4); break;
		case 10: printf("\twr(0x1c, %u);\n\trd(0x14);\n\trd(0x18);\n", rnd() % 4); break;
		case 11: printf("\twr(0x0c, 0x%x);\n\trd(0x0c);\n", rnd()); break;
		case 12: printf("\twr(0x04, 0x%x);\n\trd(0x04);\n\trd(0x14);\n", rnd()); break;
		default:
			if (waits < 3) {
				waits++;
				printf("\twait_secs(%u);\n\tdr = rd(0x00);\n\trd(0x14);\n\trd(0x18);\n", 1 + rnd() % 2);
			}
			break;
		}
	}
	printf("\tdr = rd(0x00);\n\trd(0x14);\n\trd(0x18);\n");
	return 0;
}
