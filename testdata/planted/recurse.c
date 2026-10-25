int rec_id(int n) {
    if (n <= 0)
        return 0;
    return rec_id(n - 1);
}

int rec_add(int n) {
    if (n <= 0)
        return 0;
    return rec_add(n - 1) + 1;
}

int rec_ok_base(int n) {
    if (n <= 0)
        return 0;
    return n;
}
