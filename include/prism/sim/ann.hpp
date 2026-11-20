#pragma once

// forge::ann glue for prism::sim: the sym_vec codec and the metric check.

#include <forge/ann/flat.hpp>
#include <forge/ann/fusion.hpp>
#include <forge/ann/hnsw.hpp>
#include <forge/ann/metrics.hpp>

#include "prism/sim/symbolic_metric.hpp"

namespace forge::ann {

template <> struct codec<prism::sim::sym_vec> {
    static void encode(const prism::sim::sym_vec& v, std::vector<std::byte>& out) { prism::sim::encode_sym_vec(v, out); }
    static prism::sim::sym_vec decode(std::span<const std::byte> b) { return prism::sim::decode_sym_vec(b.data(), b.size()); }
};

}  // namespace forge::ann

namespace prism::sim {
static_assert(forge::ann::metric<symbolic_metric, sym_vec>, "symbolic_metric must satisfy forge::ann::metric");
}
