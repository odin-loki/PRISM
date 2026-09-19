bool bitset_bad(unsigned n) {
    std::bitset<8> b;
    return b.test(n);
}

bool bitset_ok(unsigned n) {
    std::bitset<8> b;
    if (n >= 8)
        return false;
    return b.test(n);
}
