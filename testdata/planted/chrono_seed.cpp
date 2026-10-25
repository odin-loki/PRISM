void chrono_seed_bad(void) {
    std::mt19937 rng(std::chrono::system_clock::now().time_since_epoch().count());
}

void chrono_seed_ok(void) {
    std::mt19937 rng(1);
}
