// Force-included into prism_tests under MSVC cl only (see CMakeLists.txt).
// The PIR model headers in src/prism/pir/models/cxx are written for clang and
// use __builtin_trap on contract-violation arms; several tests include them
// by path. MSVC cl has no such builtin, so map it to std::abort.
#pragma once
#include <cstdlib>
#ifndef __builtin_trap
#  define __builtin_trap() std::abort()
#endif
