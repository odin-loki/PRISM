// PRISM conformance task virt/virt_dispatch_true.cpp: expected true (no-div0)
struct Shape {
    virtual ~Shape() = default;
    virtual int sides() const = 0;
};
struct Tri : Shape {
    int sides() const override { return 3; }
};
struct Quad : Shape {
    int sides() const override { return 4; }
};
static int count(const Shape& s) { return s.sides(); }
int virt_dispatch_true(int x) {
    Tri t;
    Quad q;
    int n = x > 0 ? count(t) : count(q);
    return 12 / (n - 2);
}
