// PRISM conformance task c17/c17_stdc_version_true.c: expected true (no-div0)
int c17_stdc_version_true(int x) {
    long v = __STDC_VERSION__ - 201710L;
    return x / (int)(v + 1);
}
