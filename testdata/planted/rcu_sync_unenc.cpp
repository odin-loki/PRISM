int rcu_sync_only_unenc_bad(int n) {
    rcu_synchronize(); return n;
}

int rcu_sync_only_unenc_ok(int n) {
    return n;
}
