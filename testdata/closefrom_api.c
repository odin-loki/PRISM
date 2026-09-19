int closefrom(int lowfd);

void closefrom_bad(void) {
    closefrom(0);
}

void closefrom_ok(void) {
    if (closefrom(0) != 0)
        return;
}
