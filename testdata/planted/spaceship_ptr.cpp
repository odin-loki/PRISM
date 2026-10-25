struct ShipPtrBad {
    int *p;
    auto operator<=>(const ShipPtrBad&) const = default;
};

int spaceship_ptr_bad(void) {
    ShipPtrBad a;
    return 0;
}

struct ShipPtrOk {
    int x;
    auto operator<=>(const ShipPtrOk&) const = default;
};

int spaceship_ptr_ok(void) {
    ShipPtrOk a;
    return 0;
}

struct ShipPtrUser {
    int *p;
    int operator<=>(const ShipPtrUser& o) const { return p == o.p ? 0 : 1; }
};

int spaceship_user_ok(void) {
    ShipPtrUser a;
    return 0;
}
