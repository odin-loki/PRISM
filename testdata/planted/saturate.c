/* Clean scalar used as a fuzz target: no crash expected. */
int saturate(int x) {
    if (x > 100) return 100;
    if (x < 0) return 0;
    return x;
}
