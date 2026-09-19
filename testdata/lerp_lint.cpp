int lerp_bad(float a, float b, float t, int *p) { return p[(int)std::lerp(a, b, t)]; }
int lerp_ok(float a, float b, float t, int *p) {
    auto k = (int)std::lerp(a, b, t);
    if (k < 0 || k > 8) return 0;
    return p[k];
}
