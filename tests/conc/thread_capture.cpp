// FAILED (race on v): two std::threads capture the same global by reference in a lambda with an argument.
#include <thread>
int v[1];
int main() {
    auto f = [&](int k) { v[0] += k; };
    std::thread a(f, 1), b(f, 2);
    a.join();
    b.join();
    return 0;
}
