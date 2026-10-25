enum Color { RED, GREEN, BLUE };

int enum_hole_bad(enum Color c) {
    switch (c) {
    case RED:
        return 1;
    case GREEN:
        return 2;
    }
    return 0;
}

int enum_hole_ok(enum Color c) {
    switch (c) {
    case RED:
        return 1;
    case GREEN:
        return 2;
    case BLUE:
        return 3;
    }
    return 0;
}

int enum_hole_default_ok(enum Color c) {
    switch (c) {
    case RED:
        return 1;
    default:
        return 0;
    }
}
