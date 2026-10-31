// Differential trace of std::visit over one variant (libstdc++ vs PRISM's
// <variant> model). The visitor sees only the active alternative.
#include <cstdio>
#include <cstdlib>
#include <variant>

extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::abort();
    }
}

struct Show {
    int operator()(int v) const { return std::printf("int %d\n", v); }
    int operator()(short v) const { return std::printf("short %d\n", static_cast<int>(v)); }
    int operator()(char v) const { return std::printf("char %c\n", v); }
};

int main() {
    std::variant<int, short, char> v = 5;
    std::visit(Show{}, v);
    v = static_cast<short>(-3);
    std::visit(Show{}, v);
    v = 'q';
    const auto& cv = v;
    std::visit(Show{}, cv);
    int r = std::visit([](auto x) { return static_cast<int>(x) * 2; }, v);
    std::printf("r %d index %zu\n", r, v.index());
    return 0;
}
