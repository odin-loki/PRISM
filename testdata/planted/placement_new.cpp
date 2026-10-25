struct PlaceT {
    int x;
};

void place_new_bad(void) {
    PlaceT *obj;
    obj = 0;
    new (obj) PlaceT;
}

void place_new_ok(void) {
    alignas(PlaceT) unsigned char buf[sizeof(PlaceT)];
    new (buf) PlaceT;
}
