int pivot_root(const char *new_root, const char *put_old);

void pivot_root_bad(void) {
    pivot_root("a","b");
}

void pivot_root_ok(void) {
    if (pivot_root("a","b")!=0)
        return;
}
