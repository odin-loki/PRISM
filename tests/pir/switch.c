/* PIR tasks: switch (lowered by lowerswitch). */
int sw_bad(int x) {
    switch (x) {
    case 1: return 10;
    case 7: return 100 / (x - 7);
    default: return 0;
    }
}
int sw_ok(int x) {
    switch (x) {
    case 1: return 10;
    case 7: return 100 / (x - 6);
    default: return 0;
    }
}
