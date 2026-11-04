#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
void ummap_find_bad(void) {
    std::unordered_multimap<int,int> m;
    auto it=m.find(1);
    (void)it->second;
}
void ummap_find_ok(void) {
    std::unordered_multimap<int,int> m;
    auto it=m.find(1);
    if (it==m.end()) return;
    (void)it->second;
}
