// F11: `throw;` inside a nested handler keeps the exception alive until the
// outermost handler that caught it ends (Itanium ABI handler count).
struct A {
  int value;
  explicit A(int v) : value(v) {}
};
struct B {
  int value;
  explicit B(int v) : value(v) {}
};

int eh_nested_rethrow_true() {
  try {
    throw A(1);
  } catch (A &) {
    try {
      throw B(2);
    } catch (B &) {
    }
    try {
      throw;
    } catch (A &a) {
      return 100 / (a.value == 1);
    }
  }
  return 0;
}
