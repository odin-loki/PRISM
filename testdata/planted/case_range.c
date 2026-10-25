int case_range_unenc_bad(int n) {
    switch (n) {
    case 1 ... 3:
        return 1;
    default:
        return 0;
    }
}

int case_range_ok(int n) {
    switch (n) {
    case 1:
        return 1;
    case 2:
        return 2;
    default:
        return 0;
    }
}
