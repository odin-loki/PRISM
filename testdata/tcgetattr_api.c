int tcgetattr(int fd, void *termios_p);

void tcgetattr_bad(void) {
    tcgetattr(0,0);
}

void tcgetattr_ok(void) {
    if (tcgetattr(0,0)!=0)
        return;
}
