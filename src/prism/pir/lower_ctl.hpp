#pragma once

// IR rewrites before opt for control-flow features (docs/PIR.md
// "setjmp/longjmp"). Called by the pir stage on the instrumented -O0 IR.

#include <string>

namespace prism::pir::pirctl {

// In every function that calls setjmp, keep the locals in memory: each
// alloca gets a `call void @__prism.keep(ptr %x)` use so mem2reg leaves it
// alone. After a longjmp the program then reads what the -O0 build reads
// (the last stored value) instead of the SSA value from the setjmp call.
std::string keep_setjmp_locals(const std::string& ir);

// Floating-point locals (`alloca half|float|double`) start from
// @__prism.uninit.<type>(), like the iN locals of the stage's instrument():
// a read before the first store stays visible after mem2reg (UNINIT-READ).
std::string uninit_fp_locals(const std::string& ir);

}  // namespace prism::pir::pirctl
