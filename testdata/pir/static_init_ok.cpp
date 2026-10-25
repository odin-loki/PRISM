// PIR: dynamic initialisation before main (docs/CONFORMANCE.md S8). The
// static-initialisation code is proved and main does not read the globals it
// sets, so main's proof stands for the program.
struct B {
    int i;
    B() : i(5) {}
};

B g;

int main() {
    int x = 2;
    return x * 3;
}
