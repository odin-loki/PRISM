int struct_local_bad(int x) {
    struct S s;
    return x;
}

int typedef_local_bad(int x) {
    item_t y;
    return x;
}

int anon_struct_bad(int x) {
    struct { int f; } s;
    return x;
}
