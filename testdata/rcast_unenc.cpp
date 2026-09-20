int rcast_unenc_bad(int n) {
    int x = n;
    float f = reinterpret_cast<float&>(x);
    return (int)f;
}

int rcast_unenc_ok(int n) {
    return n;
}
