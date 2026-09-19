int read_slot(int slot) {
    if (slot < 0)
        return -1;
    return slot;
}
int write_slot(int slot) {
    int buf[4];
    buf[slot] = 1;
    return buf[0];
}
int both_ok(int slot) {
    if (slot < 0)
        return -1;
    return slot;
}
int both_ok_write(int slot) {
    if (slot < 0)
        return -1;
    return slot;
}
