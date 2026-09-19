struct ThrowBad {
    ~ThrowBad() { throw 1; }
};

struct ThrowOk {
    ~ThrowOk() { int x = 0; (void)x; }
};

void throws_not_dtor(void) {
    throw 1;
}
