struct Base { int x; };
struct Derived : public Base { int y; };

void take_val(Base b) {
    (void)b.x;
}

void take_ref(Base &b) {
    (void)b.x;
}

void slice_bad(Derived d) {
    take_val(d);
}

void slice_ok(Derived d) {
    take_ref(d);
}

void slice_base_ok(Base b) {
    take_val(b);
}
