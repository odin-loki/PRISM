void handler(int);
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1)
#endif
int signal(int, void (*)(int));

void signal_bad(void) {
    signal(SIGINT, handler);
}

void signal_ok(void) {
    signal(SIGINT, SIG_IGN);
}

void signal_dfl_ok(void) {
    signal(SIGINT, SIG_DFL);
}
