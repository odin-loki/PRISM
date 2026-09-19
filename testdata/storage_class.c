int register_local_bad(int n) {
    register int x;
    x = n;
    return x;
}

int auto_storage_bad(int n) {
    auto int x;
    x = n;
    return x;
}

int auto_type_bad(int n) {
    auto x = n;
    return x;
}

int auto_type_gnu_bad(int n) {
    __auto_type x = n;
    return x;
}

int static_local_bad(int n) {
    static int x;
    x = n;
    return x;
}

int extern_local_bad(int n) {
    extern int x;
    x = n;
    return x;
}
