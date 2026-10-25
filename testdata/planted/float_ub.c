float fdiv_bad(float x) {
    return x / 0.0f;
}
float fdiv_ok(float x, float y) {
    if (y == 0.0f)
        return 0.0f;
    return x / y;
}
