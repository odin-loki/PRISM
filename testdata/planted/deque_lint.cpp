void deque_bad(int i) {
    std::deque<int> d(4);
    d[i]=1;
}

void deque_ok(int i) {
    std::deque<int> d(4);
    if (i>=0 && (unsigned)i<d.size()) d[i]=1;
}
