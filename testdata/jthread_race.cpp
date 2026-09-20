int g;

void jrace_t1(void) { g = 1; }
void jrace_t2(void) { g = 2; }

void jrace_start(void) {
    std::jthread a(jrace_t1);
    std::jthread b(jrace_t2);
}
