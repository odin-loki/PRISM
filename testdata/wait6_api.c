int wait6(int idtype, int id, int *status, int options,
          void *wrusage, void *infop);

void wait6_bad(void) {
    wait6(0, -1, 0, 0, 0, 0);
}

void wait6_ok(void) {
    if (wait6(0, -1, 0, 0, 0, 0) < 0)
        return;
}
