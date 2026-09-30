#pragma once

// Python's random.Random, bit for bit: MT19937 seeded by init_by_array with
// the 32-bit words of |seed| (random.seed(int)), random() from 53 bits,
// getrandbits, and randint / choice / sample exactly as CPython 3.11
// implements them (_randbelow by rejection on getrandbits). The random
// programs and input grids of the scorers are therefore the same for the
// same --seed as they were when the scorers were Python scripts, so a seed
// quoted in docs/CONFORMANCE.md still reproduces its programs.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace prism::qa {

class PyRandom {
public:
    using i128 = __int128;
    using u128 = unsigned __int128;

    explicit PyRandom(i128 seed) { this->seed(seed); }

    void seed(i128 s) {
        u128 n = s < 0 ? static_cast<u128>(-(s + 1)) + 1 : static_cast<u128>(s);
        std::vector<std::uint32_t> key;
        while (n) {
            key.push_back(static_cast<std::uint32_t>(n & 0xffffffffu));
            n >>= 32;
        }
        if (key.empty()) key.push_back(0);
        init_by_array(key);
    }

    std::uint32_t genrand() {
        static constexpr std::uint32_t mag01[2] = {0x0u, 0x9908b0dfu};
        std::uint32_t y;
        if (mti_ >= N) {
            int kk = 0;
            for (; kk < N - M; kk++) {
                y = (mt_[kk] & UPPER) | (mt_[kk + 1] & LOWER);
                mt_[kk] = mt_[kk + M] ^ (y >> 1) ^ mag01[y & 0x1u];
            }
            for (; kk < N - 1; kk++) {
                y = (mt_[kk] & UPPER) | (mt_[kk + 1] & LOWER);
                mt_[kk] = mt_[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1u];
            }
            y = (mt_[N - 1] & UPPER) | (mt_[0] & LOWER);
            mt_[N - 1] = mt_[M - 1] ^ (y >> 1) ^ mag01[y & 0x1u];
            mti_ = 0;
        }
        y = mt_[mti_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }

    // random.random(): [0, 1) from 53 bits
    double random() {
        std::uint32_t a = genrand() >> 5, b = genrand() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    // random.getrandbits(k), k <= 127
    u128 getrandbits(int k) {
        if (k <= 0) return 0;
        if (k <= 32) return genrand() >> (32 - k);
        u128 r = 0;
        int shift = 0;
        while (k > 0) {
            std::uint32_t w = genrand();
            if (k < 32) w >>= (32 - k);
            r |= static_cast<u128>(w) << shift;
            shift += 32;
            k -= 32;
        }
        return r;
    }

    // random._randbelow(n), n > 0
    u128 randbelow(u128 n) {
        int k = 0;
        for (u128 t = n; t; t >>= 1) ++k;
        u128 r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

    // random.randint(a, b), inclusive
    i128 randint(i128 a, i128 b) {
        if (b < a) throw std::invalid_argument("empty range for randint");
        return a + static_cast<i128>(randbelow(static_cast<u128>(b - a) + 1));
    }

    template <class T>
    const T& choice(const std::vector<T>& seq) {
        if (seq.empty()) throw std::invalid_argument("choice from an empty sequence");
        return seq[static_cast<std::size_t>(randbelow(seq.size()))];
    }

    // random.sample(population, k)
    template <class T>
    std::vector<T> sample(const std::vector<T>& population, std::size_t k) {
        const std::size_t n = population.size();
        if (k > n) throw std::invalid_argument("sample larger than population");
        std::vector<T> result(k);
        std::size_t setsize = 21;
        if (k > 5) setsize += static_cast<std::size_t>(std::pow(4.0, std::ceil(std::log(3.0 * static_cast<double>(k)) / std::log(4.0))));
        if (n <= setsize) {
            std::vector<T> pool(population);
            for (std::size_t i = 0; i < k; ++i) {
                auto j = static_cast<std::size_t>(randbelow(n - i));
                result[i] = pool[j];
                pool[j] = pool[n - i - 1];
            }
        } else {
            std::vector<bool> selected(n, false);
            for (std::size_t i = 0; i < k; ++i) {
                auto j = static_cast<std::size_t>(randbelow(n));
                while (selected[j]) j = static_cast<std::size_t>(randbelow(n));
                selected[j] = true;
                result[i] = population[j];
            }
        }
        return result;
    }

private:
    static constexpr int N = 624, M = 397;
    static constexpr std::uint32_t UPPER = 0x80000000u, LOWER = 0x7fffffffu;
    std::uint32_t mt_[N]{};
    int mti_ = N + 1;

    void init_genrand(std::uint32_t s) {
        mt_[0] = s;
        for (mti_ = 1; mti_ < N; mti_++)
            mt_[mti_] = 1812433253u * (mt_[mti_ - 1] ^ (mt_[mti_ - 1] >> 30)) + static_cast<std::uint32_t>(mti_);
    }

    void init_by_array(const std::vector<std::uint32_t>& key) {
        init_genrand(19650218u);
        std::size_t i = 1, j = 0;
        const std::size_t len = key.size();
        std::size_t k = N > len ? N : len;
        for (; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + static_cast<std::uint32_t>(j);
            i++;
            j++;
            if (i >= N) {
                mt_[0] = mt_[N - 1];
                i = 1;
            }
            if (j >= len) j = 0;
        }
        for (k = N - 1; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - static_cast<std::uint32_t>(i);
            i++;
            if (i >= N) {
                mt_[0] = mt_[N - 1];
                i = 1;
            }
        }
        mt_[0] = 0x80000000u;
    }
};

}  // namespace prism::qa
