// Force-included into prism_tests under MSVC cl only (see CMakeLists.txt).
// The PIR model headers in src/prism/pir/models/cxx are written for clang and
// use __builtin_trap on contract-violation arms; several tests include them
// by path. MSVC cl has no such builtin, so map it to the __fastfail intrinsic
// (no-return, from <intrin.h>). Not <cstdlib>: models/cxx/complex declares
// abort itself and a using-declaration from <cstdlib> conflicts with it.
#pragma once
#include <intrin.h>
#ifndef __builtin_trap
#  define __builtin_trap() __fastfail(7)
#endif
