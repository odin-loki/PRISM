struct DynBase {
    virtual ~DynBase() {}
};

struct DynDerived : DynBase {
    int n;
};

int dyn_null_bad(DynBase *b) {
    DynDerived *d = dynamic_cast<DynDerived *>(b);
    return d->n;
}

int dyn_null_ok(DynBase *b) {
    DynDerived *d = dynamic_cast<DynDerived *>(b);
    if (!d)
        return 0;
    return d->n;
}
