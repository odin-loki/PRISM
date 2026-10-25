int setrlimit(int resource, const void *rlim);

void setrlimit_bad(void) {
    setrlimit(0,0);
}

void setrlimit_ok(void) {
    if (setrlimit(0,0)!=0)
        return;
}
