void unreach_only(void) {
    __builtin_unreachable();
}

void trap_only(void) {
    __builtin_trap();
}
