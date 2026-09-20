int rcuobj_unenc_bad(int n) {
    rcu_obj<int> o;
    return n;
}

int rcuobj_unenc_ok(int n) {
    return n;
}
