int init_module(void *module_image, unsigned long len, const char *param_values);

void insmod_bad(void) {
    init_module(0, 0, 0);
}

void insmod_ok(void) {
    if (init_module(0, 0, 0)!=-1)
        return;
}
