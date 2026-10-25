struct Base {
    virtual void foo(void);
};

struct Derived : Base {
    void foo(void) override;
};

void virtual_dtor_bad(void) {
    Base *p = new Derived();
    delete p;
}

struct BaseOk {
    virtual ~BaseOk(void);
    virtual void foo(void);
};

void virtual_dtor_ok(void) {
    BaseOk *p = new BaseOk();
    delete p;
}
