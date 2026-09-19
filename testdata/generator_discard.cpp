void generator_discard_bad(void) {
    std::generator<int> g = gen();
}

void generator_discard_ok(void) {
    for (int x : gen())
        (void)x;
}
