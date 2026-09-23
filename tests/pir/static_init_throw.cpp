// PIR: dynamic initialisation before main (docs/CONFORMANCE.md S8). A global
// object's constructor throws before main runs (std::terminate): main's own
// body is fine, but a proof of main would not be the program's. From ESBMC
// regression/esbmc-cpp/try_catch/lower-exceptions_static_init_fail.
struct C {
    C() { throw 1; }
};

C global_c;

int main() { return 0; }
