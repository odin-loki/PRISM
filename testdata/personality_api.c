int personality(unsigned persona);

void personality_bad(void) {
    personality(0);
}

void personality_ok(void) {
    if (personality(0)==-1)
        return;
}
