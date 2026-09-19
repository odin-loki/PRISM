/* 1<<31 into a signed int is UB. Lint should find it; BMC too if it parses. */
int shift_ub(int x) {
    return 1 << 31;
}
