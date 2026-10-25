int rcu_bad(void) {
    std::rcu_obj<int> o;
    o = 1;
    return 0;
}

int rcu_ok(void) {
    std::rcu_obj<int> o;
    o = 1;
    rcu_synchronize();
    return 0;
}
