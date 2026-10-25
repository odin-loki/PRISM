int io_setup(void);

void io_setup_bad(void) {
    io_setup();
}

void io_setup_ok(void) {
    if (io_setup()!=-1)
        return;
}
