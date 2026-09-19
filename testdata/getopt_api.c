int getopt(int argc, char *const argv[], const char *optstring);

void getopt_bad(void) {
    getopt(0,0,0);
}

void getopt_ok(void) {
    if (getopt(0,0,0)<0)
        return;
}
