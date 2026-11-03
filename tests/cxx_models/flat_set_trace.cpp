// Trace of std::flat_set. Include <flat_set> and use insert, contains, find,
// and erase. A second insert of the same key does not grow size. find of a
// missing key is end. erase of a missing key leaves size unchanged.
// tests/cpp/test_cxx_flat_set.cpp checks the model text; it does not shell
// out to this file.
#include <cstdio>
#include <flat_set>

// The models call __prism_check, the precondition intrinsic. libstdc++ never
// calls it. A failed check aborts so the model and libstdc++ runs still differ.
extern "C" __attribute__((weak)) void __prism_check(int ok, const char*, const char*) {
    if (!ok) __builtin_abort();
}

int main() {
    std::flat_set<int> empty;
    std::printf("empty_eq %d\n", empty.begin() == empty.end() ? 1 : 0);
    std::printf("empty_size %d\n", static_cast<int>(empty.size()));

    std::flat_set<int> s;
    s.insert(2);
    s.insert(1);
    auto dup = s.insert(2);
    std::printf("dup_inserted %d size %d\n", dup.second ? 1 : 0, static_cast<int>(s.size()));
    std::printf("contains1 %d contains3 %d\n", s.contains(1) ? 1 : 0, s.contains(3) ? 1 : 0);
    std::printf("find3_end %d\n", s.find(3) == s.end() ? 1 : 0);
    std::printf("order");
    for (int k : s)
        std::printf(" %d", k);
    std::printf("\n");
    std::printf("erase_missing %d size %d\n", static_cast<int>(s.erase(3)), static_cast<int>(s.size()));
    std::printf("erase_present %d size %d\n", static_cast<int>(s.erase(2)), static_cast<int>(s.size()));
    std::printf("contains1 %d contains2 %d\n", s.contains(1) ? 1 : 0, s.contains(2) ? 1 : 0);
    return 0;
}
