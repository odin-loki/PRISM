void slock_bad(void) {
    std::shared_mutex m;
    std::shared_lock<std::shared_mutex> lk; /* default, not owning */
    (void)lk.mutex();
}

void slock_ok(void) {
    std::shared_mutex m;
    std::shared_lock<std::shared_mutex> lk(m);
    if (!lk.owns_lock()) return;
}
