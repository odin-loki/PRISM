int fall_bad(int x) {
    int y = 0;
    switch (x) {
    case 1:
        y = 1;
    case 2:
        y = 2;
        break;
    }
    return y;
}

int fall_ok(int x) {
    int y = 0;
    switch (x) {
    case 1:
        y = 1;
        break;
    case 2:
        y = 2;
        break;
    }
    return y;
}
