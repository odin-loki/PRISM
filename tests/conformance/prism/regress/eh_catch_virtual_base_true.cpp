// F11: a base reached through a virtual base has a run-time offset; PRISM
// must not bind it as if it were at offset 0 (no proof either way).
struct V {
  int v;
  V() : v(1) {}
  virtual ~V() {}
};
struct Pad {
  int pad[4];
  Pad() : pad() {}
  virtual ~Pad() {}
};
struct D : Pad, virtual V {};

int eh_catch_virtual_base_true() {
  try {
    throw D();
  } catch (V &x) {
    return 100 / (x.v == 1);
  }
  return 0;
}
