// libFuzzer launch status (src/prism/stages/fuse.cpp). libfuzzer_launch_status
// maps a ProcRun and a crash-file sample. It does not spawn libFuzzer. A
// process that never started is NOTRUN (Law 1), never CLEAN. A started run
// with no crash file and no sanitizer report is CLEAN and not a proof
// (Law 3), including a timeout or an exit of 127. A crash file or an
// ASan/UBSan report is CRASH.

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

prism::FunctionInfo lf_fn() {
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

TEST_CASE("libfuzzer: a launch that never started is NOTRUN, never CLEAN") {
    prism::stages_detail::ProcRun run;
    run.failed = true;
    run.rc = 127;
    run.err = "cannot execute lfuzzer_inc.exe: No such file or directory";

    auto f = prism::stages_detail::libfuzzer_launch_status(lf_fn(), run, 2, std::nullopt);
    CHECK(f.stage == "libfuzzer");
    CHECK(f.status == prism::laws::NOTRUN);
    CHECK(f.status != prism::laws::CLEAN);
    CHECK(f.status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message == run.err);
    CHECK(f.extra.at("install") == "clang libFuzzer did not start");
    CHECK_FALSE(f.extra.contains("engine"));

    prism::stages_detail::ProcRun bare;
    bare.failed = true;
    bare.rc = 127;
    auto fallback = prism::stages_detail::libfuzzer_launch_status(lf_fn(), bare, 2, std::nullopt);
    CHECK(fallback.status == prism::laws::NOTRUN);
    CHECK(fallback.status != prism::laws::CLEAN);
    CHECK(fallback.message == "failed to start");
    CHECK(fallback.extra.at("install") == "clang libFuzzer did not start");

    // A crash file or a sanitizer line does not turn an unstarted process
    // into CLEAN or CRASH. Only ProcRun::failed decides NOTRUN.
    prism::stages_detail::LibfuzzerCrashFile leftover;
    leftover.bytes = "x";
    leftover.count = 1;
    auto with_file = prism::stages_detail::libfuzzer_launch_status(lf_fn(), run, 2, leftover);
    CHECK(with_file.status == prism::laws::NOTRUN);
    CHECK(with_file.status != prism::laws::CLEAN);
    CHECK(with_file.status != prism::laws::CRASH);
    CHECK(with_file.message == run.err);

    prism::stages_detail::ProcRun poisoned;
    poisoned.failed = true;
    poisoned.rc = 127;
    poisoned.err = "AddressSanitizer: heap-buffer-overflow";
    auto with_san = prism::stages_detail::libfuzzer_launch_status(lf_fn(), poisoned, 2, std::nullopt);
    CHECK(with_san.status == prism::laws::NOTRUN);
    CHECK(with_san.status != prism::laws::CRASH);
    CHECK(with_san.message == poisoned.err);
    CHECK(with_san.extra.at("install") == "clang libFuzzer did not start");
}

TEST_CASE("libfuzzer: a started run with no crash file is CLEAN and not a proof") {
    const prism::FunctionInfo fn = lf_fn();

    prism::stages_detail::ProcRun finished;
    finished.failed = false;
    finished.rc = 0;
    auto quiet = prism::stages_detail::libfuzzer_launch_status(fn, finished, 2, std::nullopt);
    CHECK(quiet.stage == "libfuzzer");
    CHECK(quiet.status == prism::laws::CLEAN);
    CHECK(quiet.status != prism::laws::NOTRUN);
    CHECK(quiet.status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(quiet.status));
    CHECK(quiet.message == "no libFuzzer crash in 2s (not a proof)");
    CHECK(quiet.message.find("not a proof") != std::string::npos);
    CHECK(quiet.extra.at("engine") == "libfuzzer");
    CHECK(quiet.extra.at("sandbox") == prism::sandbox::kind());

    // A timeout is a started campaign that found nothing, not a missing tool.
    prism::stages_detail::ProcRun timed_out;
    timed_out.failed = false;
    timed_out.timeout = true;
    timed_out.rc = -1;
    auto held = prism::stages_detail::libfuzzer_launch_status(fn, timed_out, 2, std::nullopt);
    CHECK(held.status == prism::laws::CLEAN);
    CHECK(held.status != prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(held.status));
    CHECK(held.message.find("not a proof") != std::string::npos);

    // A non-zero exit from a child that did start is not ProcRun::failed.
    prism::stages_detail::ProcRun exited;
    exited.failed = false;
    exited.rc = 1;
    exited.err = "ERROR: libFuzzer: timeout";
    auto nonzero = prism::stages_detail::libfuzzer_launch_status(fn, exited, 2, std::nullopt);
    CHECK(nonzero.status == prism::laws::CLEAN);
    CHECK(nonzero.status != prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(nonzero.status));
    CHECK(nonzero.message.find("not a proof") != std::string::npos);

    // rc 127 from a child that did start is not NOTRUN.
    prism::stages_detail::ProcRun usage;
    usage.failed = false;
    usage.rc = 127;
    usage.err = "usage";
    auto started_127 = prism::stages_detail::libfuzzer_launch_status(fn, usage, 2, std::nullopt);
    CHECK(started_127.status == prism::laws::CLEAN);
    CHECK(started_127.status != prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(started_127.status));
    CHECK(started_127.message.find("not a proof") != std::string::npos);
}

TEST_CASE("libfuzzer: a started run that left a crash file is CRASH") {
    // Stands in for crash-<sha> (plus a second file). The helper does not
    // open the path and does not spawn libFuzzer. Seventeen bytes keep the
    // message on the first 16 and the counterexample on the whole file.
    prism::stages_detail::LibfuzzerCrashFile crash;
    crash.bytes.assign(17, '\0');
    for (int i = 0; i < 17; ++i) crash.bytes[static_cast<std::size_t>(i)] = static_cast<char>(i + 1);
    crash.count = 2;

    prism::stages_detail::ProcRun started;
    started.failed = false;
    started.rc = 1;
    auto f = prism::stages_detail::libfuzzer_launch_status(lf_fn(), started, 2, crash);
    CHECK(f.stage == "libfuzzer");
    CHECK(f.status == prism::laws::CRASH);
    CHECK(f.status != prism::laws::CLEAN);
    CHECK(f.status != prism::laws::NOTRUN);
    CHECK(f.cls == "LIBFUZZER-CRASH");
    CHECK(f.message == "libFuzzer crash on 0102030405060708090a0b0c0d0e0f10");
    CHECK(f.counterexample == "0102030405060708090a0b0c0d0e0f1011");
    CHECK(f.extra.at("libfuzzer_crashes") == "2");
    CHECK(f.extra.at("engine") == "libfuzzer");
    CHECK(f.extra.at("sandbox") == prism::sandbox::kind());
    CHECK_FALSE(prism::laws::is_proof(f.status));
}

TEST_CASE("libfuzzer: an ASan or UBSan report with no crash file is CRASH") {
    const prism::FunctionInfo fn = lf_fn();

    prism::stages_detail::ProcRun asan;
    asan.failed = false;
    asan.rc = 1;
    asan.err = "AddressSanitizer: heap-buffer-overflow";
    auto a = prism::stages_detail::libfuzzer_launch_status(fn, asan, 2, std::nullopt);
    CHECK(a.stage == "libfuzzer");
    CHECK(a.status == prism::laws::CRASH);
    CHECK(a.status != prism::laws::CLEAN);
    CHECK(a.status != prism::laws::NOTRUN);
    CHECK(a.cls == "LIBFUZZER-CRASH");
    CHECK(a.message == "libFuzzer sanitizer crash");
    CHECK(a.extra.at("engine") == "libfuzzer");

    // rc 127 from a child that did start, with a sanitizer report, stays CRASH.
    prism::stages_detail::ProcRun ubsan;
    ubsan.failed = false;
    ubsan.rc = 127;
    ubsan.out = "UndefinedBehaviorSanitizer: undefined-behavior";
    auto u = prism::stages_detail::libfuzzer_launch_status(fn, ubsan, 2, std::nullopt);
    CHECK(u.status == prism::laws::CRASH);
    CHECK(u.status != prism::laws::NOTRUN);
    CHECK(u.status != prism::laws::CLEAN);
    CHECK(u.cls == "LIBFUZZER-CRASH");
    CHECK(u.message == "libFuzzer sanitizer crash");
}
