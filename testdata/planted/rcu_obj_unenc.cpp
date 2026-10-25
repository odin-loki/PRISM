int rcu_obj_lt_unenc_bad(int n) {
    rcu_obj<int> x; return n;
}

int rcu_obj_lt_unenc_ok(int n) {
    return n;
}
