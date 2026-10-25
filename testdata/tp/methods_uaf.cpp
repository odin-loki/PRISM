// Use after free inside forms the old parser missed or misnamed: an
// out-of-line method, an in-class method, and a namespaced function.
#include <cstdlib>

struct W {
    int go();
    int inl() {
        char *q = static_cast<char*>(std::malloc(4));
        if (!q) return 0;
        std::free(q);
        return q[0];
    }
    int one_line() { return v + 1; }
    int v = 0;
};

int W::go() {
    char *p = static_cast<char*>(std::malloc(4));
    if (!p) return 0;
    free(p);
    p[0] = 1;
    return v;
}

namespace ns {
int inner(int c) {
    char *p = static_cast<char*>(std::malloc(4));
    if (!p) return 0;
    free(p);
    return p[c];
}
}  // namespace ns
