// SHA-256 and SHA-1 (FIPS 180-4) and RFC 4122 UUIDv5. Standard library only.
#include "prism/deps.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace prism::deps {

namespace {
std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
}
constexpr const char* kHex = "0123456789abcdef";
}  // namespace

void Sha256::block(const unsigned char* p) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
        auto t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
}

void Sha256::update(const void* data, std::size_t n) {
    auto p = static_cast<const unsigned char*>(data);
    bits_ += std::uint64_t(n) * 8;
    while (n > 0) {
        std::size_t take = std::min(n, sizeof(buf_) - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take;
        p += take;
        n -= take;
        if (used_ == sizeof(buf_)) {
            block(buf_);
            used_ = 0;
        }
    }
}

std::string Sha256::hex() {
    auto total = bits_;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char zero = 0;
    while (used_ != 56) update(&zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(total >> (56 - 8 * i));
    update(len, 8);
    std::string out;
    for (auto v : h_)
        for (int s = 28; s >= 0; s -= 4) out.push_back(kHex[(v >> s) & 0xf]);
    return out;
}

std::string sha256_hex(std::string_view data) {
    Sha256 s;
    s.update(data);
    return s.hex();
}

std::string sha256_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw FetchError("cannot read " + p.string());
    Sha256 s;
    std::vector<char> chunk(1 << 20);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        auto got = in.gcount();
        if (got > 0) s.update(chunk.data(), static_cast<std::size_t>(got));
    }
    if (in.bad()) throw FetchError("read error on " + p.string());
    return s.hex();
}

std::string sha1_bytes(std::string_view data) {
    std::uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    std::string msg(data);
    const std::uint64_t bits = std::uint64_t(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        auto p = reinterpret_cast<const unsigned char*>(msg.data() + off);
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            auto t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (auto v : h)
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((v >> s) & 0xff));
    return out;
}

std::string uuid5(std::string_view ns_uuid, std::string_view name) {
    std::string ns;
    for (std::size_t i = 0; i < ns_uuid.size(); ++i) {
        if (ns_uuid[i] == '-') continue;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (i + 1 >= ns_uuid.size() || nib(ns_uuid[i]) < 0 || nib(ns_uuid[i + 1]) < 0)
            throw std::invalid_argument("uuid5: bad namespace");
        ns.push_back(static_cast<char>(nib(ns_uuid[i]) * 16 + nib(ns_uuid[i + 1])));
        ++i;
    }
    if (ns.size() != 16) throw std::invalid_argument("uuid5: bad namespace");
    std::string d = sha1_bytes(ns + std::string(name)).substr(0, 16);
    d[6] = static_cast<char>((static_cast<unsigned char>(d[6]) & 0x0f) | 0x50);
    d[8] = static_cast<char>((static_cast<unsigned char>(d[8]) & 0x3f) | 0x80);
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        auto b = static_cast<unsigned char>(d[i]);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

}  // namespace prism::deps
