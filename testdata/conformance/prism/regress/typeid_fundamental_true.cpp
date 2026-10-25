#include <cstring>
#include <typeinfo>
// F11: the C++ runtime's typeinfo objects for fundamental types:
// name() is the mangled type, typeid equality compares it.
int typeid_fundamental_true() {
  int ok = std::strcmp(typeid(int).name(), "i") == 0 && typeid(int) != typeid(double);
  return 100 / (ok && typeid(long) == typeid(long));
}
