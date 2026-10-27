// Package-manager checkout directories are exact skip names.

#include <doctest/doctest.h>

#include "prism/scope.hpp"

TEST_CASE("scope: Pods, Carthage, .swiftpm, and .bundle are exact skip names") {
    for (auto* n : {"Pods", "Carthage", ".swiftpm", ".bundle"})
        CHECK_MESSAGE(prism::scope::skip_dir(n), n);
    for (auto* n : {"myPods", "swiftpm", "bundle", "PodsOld", "src", "docs"})
        CHECK_MESSAGE(!prism::scope::skip_dir(n), n);
}
