struct CxxBase {
    virtual int v() { return 0; }
};

struct CxxDer : CxxBase {
    int v() override { return 1; }
};

int dyn_cast_bad(int n) {
    CxxDer d;
    CxxBase *p = &d;
    CxxDer *q = dynamic_cast<CxxDer*>(p);
    return q ? n : 0;
}

int typeid_bad(int n) {
    CxxDer d;
    CxxBase *p = &d;
    (void)typeid(*p);
    return n;
}

int reinterp_bad(int n) {
    int x = n;
    float f = reinterpret_cast<float&>(x);
    return (int)f;
}

int static_cast_ok(int n) {
    return static_cast<int>(n);
}
