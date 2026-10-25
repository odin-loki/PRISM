unsigned long map_shadow_stack(unsigned long addr, unsigned long size, unsigned int flags);

void mshadow_bad(void) {
    map_shadow_stack(0,0,0);
}

void mshadow_ok(void) {
    if (map_shadow_stack(0,0,0)!=-1)
        return;
}
