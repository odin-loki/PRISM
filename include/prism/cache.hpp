#pragma once

// PRISM's thin content-addressed cache interface, built like log.hpp. A
// store is a directory; keys are 64-hex-digit digests of the value or of a
// caller-built key string. The calls mirror FORGE's forge::cache::store
// (support/cache/cache.hpp: put/get/contains/erase/prune/stats, BLAKE3 keys),
// so the FORGE switch is set_backend() with a forge::cache::store per
// directory and to_hex(blake3) keys. The default backend is a file store
// keyed by SHA-256 (prism::ai::sha256_hex), files at DIR/ab/cdef...

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

struct Backend {
    std::string (*key_of)(std::string_view data) = nullptr;  // 64 lowercase hex digits
    bool (*put)(const std::filesystem::path& dir, const std::string& key, std::string_view value) = nullptr;
    std::optional<std::string> (*get)(const std::filesystem::path& dir, const std::string& key) = nullptr;
    bool (*erase)(const std::filesystem::path& dir, const std::string& key) = nullptr;
    std::uint64_t (*prune)(const std::filesystem::path& dir, std::uint64_t max_bytes) = nullptr;  // bytes freed
    Stats (*stats)(const std::filesystem::path& dir) = nullptr;
};

// Replace the backend (a default-constructed Backend restores the default).
PRISM_API Backend set_backend(Backend b);

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
