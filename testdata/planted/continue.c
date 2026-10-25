/* continue skips the overflowing assignment. If continue is ignored, FAILED. */

int cont_skip(int x) {
    int i;
    i = 0;
    while (i < 4) {
        i = i + 1;
        continue;
        x = x + 100;
    }
    return x;
}
