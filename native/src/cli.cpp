#include "helix/simd.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    std::string s = (argc > 1) ? argv[1] : "helix";
    auto h = helix::coverage_hash(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    std::printf("coverage_hash(%s) = %llu\n", s.c_str(), (unsigned long long)h);
    std::vector<uint8_t> buf(16, 0);
    helix::havoc(buf.data(), buf.size(), 42);
    std::printf("havoc: ");
    for (auto b : buf) std::printf("%02x", b);
    std::printf("\n");
    return 0;
}
