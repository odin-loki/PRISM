int rcu_unenc_bad(int n) {
    std::rcu r;
    rcu_synchronize();
    return n;
}

int rcu_unenc_ok(int n) {
    return n;
}
