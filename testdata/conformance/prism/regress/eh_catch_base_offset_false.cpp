// F11: a handler that catches a base at a non-zero offset of the thrown
// object's type binds that subobject (Itanium ABI adjusted pointer).
struct A {
  int a;
  A() : a(1) {}
};
struct P {
  virtual ~P() {}
  int p;
  P() : p(9) {}
};
struct PA : P, A {};

int eh_catch_base_offset_false() {
  try {
    throw PA();
  } catch (A &a) {
    return 100 / (a.a == 9);
  }
  return 0;
}
