int tls_local_bad(int n) {
    _Thread_local int x = n;
    return x;
}

int thread_local_bad(int n) {
    thread_local int x = n;
    return x;
}

int tls_local_ok(int n) {
    int x = n;
    return x;
}
