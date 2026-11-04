#pragma once

// PRISM's content-addressed cache: a thin interface over FORGE's
// forge::cache::store (support/cache/cache.hpp). A store is a directory;
// keys are 64-hex-digit BLAKE3 digests (forge::cache::to_hex), files at
// DIR/ab/cdef..., each checked against its stored digest on read.

#include "prism/export.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace prism::cache {

struct Stats {
    std::uint64_t entries = 0, bytes = 0, hits = 0, misses = 0, evictions = 0, corrupt = 0;
};

PRISM_API std::string key_of(std::string_view data);
PRISM_API bool put(const std::filesystem::path& dir, const std::string& key, std::string_view value);
PRISM_API std::optional<std::string> get(const std::filesystem::path& dir, const std::string& key);
PRISM_API bool contains(const std::filesystem::path& dir, const std::string& key);
PRISM_API bool erase(const std::filesystem::path& dir, const std::string& key);
PRISM_API std::uint64_t prune(const std::filesystem::path& dir, std::uint64_t max_bytes);
PRISM_API Stats stats(const std::filesystem::path& dir);

// Content-addressed put: key = key_of(value).
inline std::string put(const std::filesystem::path& dir, std::string_view value) {
    auto k = key_of(value);
    put(dir, k, value);
    return k;
}

}  // namespace prism::cache
