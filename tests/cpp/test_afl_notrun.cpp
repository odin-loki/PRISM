// AFL++ launch status (src/prism/stages/fuse.cpp). afl_launch_status maps a
// ProcRun and a crash-file sample. It does not spawn afl-fuzz. A process
// that never started is NOTRUN (Law 1), never CLEAN. A started run with no
// crash file is CLEAN and not a proof (Law 3), including a timeout or an
// exit of 127. A started run that left a crash file is CRASH.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/sandbox.hpp"

#include "../../src/prism/stages/common.hpp"

#ifdef ERROR
#  undef ERROR
#endif

#include <optional>
#include <string>

namespace {

prism::FunctionInfo afl_fn() {
    prism::FunctionInfo fn;
    fn.file = "planted.c";
    fn.name = "inc";
    fn.kind = "SCALAR";
    fn.line = 1;
    fn.signature = "int inc(int x)";
    fn.params = {{"int", "x"}};
    return fn;
}

}  // namespace

TEST_CASE("afl: a launch that never started is NOTRUN, never CLEAN") {
    prism::stages_detail::ProcRun run;
    run.failed = true;
    run.rc = 127;
    run.err = "cannot execute afl-fuzz: No such file or directory";

    auto f = prism::stages_detail::afl_launch_status(afl_fn(), run, 2, std::nullopt);
    CHECK(f.stage == "fuse");
    CHECK(f.status == prism::laws::NOTRUN);
    CHECK(f.status != prism::laws::CLEAN);
    CHECK(f.status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message == run.err);
    CHECK(f.extra.at("install") == "install afl-fuzz");
    CHECK_FALSE(f.extra.contains("engine"));

    prism::stages_detail::ProcRun bare;
    bare.failed = true;
    bare.rc = 127;
    auto fallback = prism::stages_detail::afl_launch_status(afl_fn(), bare, 2, std::nullopt);
    CHECK(fallback.status == prism::laws::NOTRUN);
    CHECK(fallback.status != prism::laws::CLEAN);
    CHECK(fallback.message == "failed to start");
    CHECK(fallback.extra.at("install") == "install afl-fuzz");
}

TEST_CASE("afl: a started run with no crash file is CLEAN and not a proof") {
    const prism::FunctionInfo fn = afl_fn();

    prism::stages_detail::ProcRun finished;
    finished.failed = false;
    finished.rc = 0;
    auto quiet = prism::stages_detail::afl_launch_status(fn, finished, 2, std::nullopt);
    CHECK(quiet.stage == "fuse");
    CHECK(quiet.status == prism::laws::CLEAN);
    CHECK(quiet.status != prism::laws::NOTRUN);
    CHECK(quiet.status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(quiet.status));
    CHECK(quiet.message == "no AFL crash in 2s (not a proof)");
    CHECK(quiet.message.find("not a proof") != std::string::npos);
    CHECK(quiet.extra.at("engine") == "afl");
    CHECK(quiet.extra.at("sandbox") == prism::sandbox::kind());

    // A timeout is a started campaign that found nothing, not a missing tool.
    prism::stages_detail::ProcRun timed_out;
    timed_out.failed = false;
    timed_out.timeout = true;
    timed_out.rc = -1;
    auto held = prism::stages_detail::afl_launch_status(fn, timed_out, 2, std::nullopt);
    CHECK(held.status == prism::laws::CLEAN);
    CHECK(held.status != prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(held.status));
    CHECK(held.message.find("not a proof") != std::string::npos);

    // rc 127 from a child that did start is not ProcRun::failed.
    prism::stages_detail::ProcRun exited;
    exited.failed = false;
    exited.rc = 127;
    exited.err = "usage";
    auto usage = prism::stages_detail::afl_launch_status(fn, exited, 2, std::nullopt);
    CHECK(usage.status == prism::laws::CLEAN);
    CHECK(usage.status != prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(usage.status));
    CHECK(usage.message.find("not a proof") != std::string::npos);
}

TEST_CASE("afl: a started run that left a crash file is CRASH") {
    // Stands in for out/crashes/id:000000 (plus a second file). The helper
    // does not open the path and does not spawn afl-fuzz. Seventeen bytes
    // keep the message on the first 16 and the counterexample on the whole file.
    prism::stages_detail::AflCrashFile crash;
    crash.bytes.assign(17, '\0');
    for (int i = 0; i < 17; ++i) crash.bytes[static_cast<std::size_t>(i)] = static_cast<char>(i + 1);
    crash.count = 2;

    prism::stages_detail::ProcRun started;
    started.failed = false;
    started.rc = 0;
    auto f = prism::stages_detail::afl_launch_status(afl_fn(), started, 2, crash);
    CHECK(f.stage == "fuse");
    CHECK(f.status == prism::laws::CRASH);
    CHECK(f.status != prism::laws::CLEAN);
    CHECK(f.status != prism::laws::NOTRUN);
    CHECK(f.cls == "AFL-CRASH");
    CHECK(f.message == "AFL crash on 0102030405060708090a0b0c0d0e0f10");
    CHECK(f.counterexample == "0102030405060708090a0b0c0d0e0f1011");
    CHECK(f.extra.at("afl_crashes") == "2");
    CHECK(f.extra.at("engine") == "afl");
    CHECK(f.extra.at("sandbox") == prism::sandbox::kind());
    CHECK_FALSE(prism::laws::is_proof(f.status));
}
