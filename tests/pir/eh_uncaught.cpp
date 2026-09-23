// PIR task: an exception that leaves main calls std::terminate (CXX-UNCAUGHT).
extern "C" int __VERIFIER_nondet_int(void);

struct Negative {
    int value;
};

static int parse(int c) {
    if (c < 0) throw Negative{c};
    return c;
}

int main() { return parse(__VERIFIER_nondet_int()); }
