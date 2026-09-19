atomic_ref atomic_ref_bad(void) {
    int local;
    return atomic_ref<int>(local);
}

int atomic_ref_ok(atomic_ref r) {
    return r.load();
}
