int spaceship_unenc_bad(int n) {
    return n <=> 0;
}

int spaceship_ok(int n) {
    return n <= 0 ? 0 : n;
}
