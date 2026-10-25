struct Foo {
    char *p;
};

void assign_bad(Foo& self, const Foo& o) {
    delete self.p;
    self.p = o.p;
}

void assign_ok(Foo& self, const Foo& o) {
    if (&self == &o)
        return;
    delete self.p;
    self.p = o.p;
}
