/* Signed index bounded above only: if (i > n) return; never i < 0. */

int onesided_index(int i, int n) {
    int buf[8];
    if (i > n)
        return 0;
    return buf[i];
}

int two_sided_index(int i, int n) {
    int buf[8];
    if (i < 0)
        return 0;
    if (i > n)
        return 0;
    return buf[i];
}

int unsigned_index(unsigned i, unsigned n) {
    int buf[8];
    if (i > n)
        return 0;
    return buf[i];
}
