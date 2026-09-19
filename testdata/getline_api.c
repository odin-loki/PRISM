long getline(char **lineptr, unsigned long *n, void *stream);

void getline_bad(void) {
    getline(0,0,0);
}

void getline_ok(void) {
    if (getline(0,0,0)<0)
        return;
}
