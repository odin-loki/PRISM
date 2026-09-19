struct ThisCap {
    int x;
    auto this_capture_bad() {
        return [=]{ return x; };
    }
};

auto this_capture_ok(int x) {
    return [x]{ return x; };
}
