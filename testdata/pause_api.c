int pause(void);

void pause_bad(void) {
    pause();
}

void pause_ok(void) {
    if (pause() != 0)
        return;
}
