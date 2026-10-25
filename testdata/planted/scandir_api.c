int scandir(const char *dir, void *namelist, void *filter, void *compar);

void scandir_bad(void) {
    scandir(".",0,0,0);
}

void scandir_ok(void) {
    if (scandir(".",0,0,0)<0)
        return;
}
