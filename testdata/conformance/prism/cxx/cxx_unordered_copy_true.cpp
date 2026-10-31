// PRISM conformance task cxx/cxx_unordered_copy_true.cpp: expected true (no-overflow)
#include <unordered_map>
int cxx_unordered_copy_true(short x) {
    std::unordered_map<int, int> a;
    for (int i = 0; i < 20; ++i) a[i] = i;
    auto b = a;
    return static_cast<int>(b.size()) + x + (b.bucket_count() == a.bucket_count() ? 1 : 0);
}
