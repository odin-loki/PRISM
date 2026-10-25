int remap_file_pages(void *addr, unsigned long size, int prot, unsigned long pgoff, int flags);

void remap_bad(void) {
    remap_file_pages(0,0,0,0,0);
}

void remap_ok(void) {
    if (remap_file_pages(0,0,0,0,0)!=-1)
        return;
}
