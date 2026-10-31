// Force-included into prism_tests under MSVC cl only (see CMakeLists.txt).
// The PIR model headers in src/prism/pir/models/cxx are written for clang and
// use __builtin_trap on contract-violation arms; several tests include them
// by path. MSVC cl has no such builtin, so this gives the same no-return
// stand-in models/cxx/complex defines for itself (which then skips its own
// copy, see PRISM_MSVC_BUILTIN_TRAP). abort is declared exactly as the UCRT
// declares it, so a later <stdlib.h> does not conflict.
#pragma once
#define PRISM_MSVC_BUILTIN_TRAP 1
#if defined(_DLL)
extern "C" __declspec(dllimport) __declspec(noreturn) void __cdecl abort(void);
#else
extern "C" __declspec(noreturn) void __cdecl abort(void);
#endif
__declspec(noreturn) inline void __builtin_trap() { abort(); }
