// F11: catch (T*) binds the thrown pointer's value, not the exception object.
struct A {
  int x;
};

int eh_catch_pointer_false() {
  A a;
  a.x = 5;
  try {
    throw &a;
  } catch (A *p) {
    return 100 / (p == &a && p->x == 6);
  }
  return 0;
}
