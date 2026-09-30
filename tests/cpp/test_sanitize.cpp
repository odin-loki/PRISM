// Doctests for the sanitize stage (src/prism/adapters.cpp run_sanitize):
// a missing compiler or sanitizer is NOTRUN, a probe that only accepts the
// flag is not support, CLEAN is not a proof, an abort is FAILED, and an
// unusable runtime is NOTRUN, not a defect. The compiler is a shell script
// on a temporary PATH, so every branch runs without a real toolchain.

#include <doctest/doctest.h>

#ifndef _WIN32

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/sanitize_detail.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace sd = prism::sanitize_detail;

const std::vector<std::string> kUb{"-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-O0"};
const std::vector<std::string> kAs{"-fsanitize=address", "-fno-sanitize-recover=address", "-O0"};
const std::vector<std::string> kTs{"-fsanitize=thread", "-O0"};

// The fake compiler. -dumpmachine prints $FAKE_TRIPLE; -print-file-name
// echoes its argument (what a compiler without the runtime does). A build
// with -fsanitize=X fails unless X is in $FAKE_SAN; otherwise it writes an
// executable script to -o: the probe's fire programs report the sanitizer
// (unless $FAKE_NOFIRE), and any other program runs $FAKE_RUN_<X> (default
// `exit 0`). Every build is logged to $FAKE_LOG.
const char* kFakeCc = R"(#!/bin/sh
case "$1" in
  -dumpmachine) echo "${FAKE_TRIPLE:-x86_64-linux-gnu}"; exit 0;;
  -print-file-name=*) echo "${1#-print-file-name=}"; exit 0;;
esac
out=""; san=""; prev=""; fire=0
for a in "$@"; do
  [ "$prev" = "-o" ] && out="$a"
  case "$a" in
    -fsanitize=*) san="${a#-fsanitize=}";;
    *ub_fire.c|*as_fire.c) fire=1;;
  esac
  prev="$a"
done
[ -n "$FAKE_LOG" ] && echo "build $san $fire" >> "$FAKE_LOG"
if [ -n "$san" ]; then
  case " $FAKE_SAN " in *" $san "*) ;; *) echo "unsupported -fsanitize=$san" >&2; exit 1;; esac
fi
if [ "$fire" = 1 ] && [ -z "$FAKE_NOFIRE" ]; then
  case "$san" in
    undefined) body='echo "x.c:3: runtime error: signed integer overflow" >&2; exit 1';;
    *) body='echo "ERROR: AddressSanitizer: heap-buffer-overflow" >&2; exit 1';;
  esac
elif [ "$fire" = 1 ]; then
  body='exit 0'
else
  eval "body=\${FAKE_RUN_$san:-exit 0}"
fi
printf '#!/bin/sh\n%s\n' "$body" > "$out"
chmod +x "$out"
exit 0
)";

// A scratch tree with a fake compiler at <dir>/<bin>/gcc on PATH, and the
// environment restored on exit.
struct FakeCc {
    fs::path dir;
    fs::path cc;
    std::map<std::string, std::optional<std::string>> saved;
    explicit FakeCc(const std::string& bin = "bin") {
        dir = fs::temp_directory_path() / ("prism_san_fake_" + std::to_string(::getpid()) + "_" + bin);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / bin);
        cc = dir / bin / "gcc";
        std::ofstream(cc) << kFakeCc;
        fs::permissions(cc, fs::perms::owner_all);
        set("PATH", (dir / bin).string() + ":/usr/bin:/bin");
        for (auto* k : {"FAKE_SAN", "FAKE_TRIPLE", "FAKE_NOFIRE", "FAKE_RUN_undefined", "FAKE_RUN_address",
                        "FAKE_RUN_thread"})
            unset(k);
        set("FAKE_LOG", (dir / "log").string());
    }
    ~FakeCc() {
        for (auto& [k, v] : saved) {
            if (v) ::setenv(k.c_str(), v->c_str(), 1);
            else ::unsetenv(k.c_str());
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void remember(const std::string& k) {
        if (saved.contains(k)) return;
        const char* v = std::getenv(k.c_str());
        saved[k] = v ? std::optional<std::string>(v) : std::nullopt;
    }
    void set(const std::string& k, const std::string& v) {
        remember(k);
        ::setenv(k.c_str(), v.c_str(), 1);
    }
    void unset(const std::string& k) {
        remember(k);
        ::unsetenv(k.c_str());
    }
    fs::path put(const std::string& name, const std::string& text) {
        auto p = dir / name;
        std::ofstream(p) << text;
        return p;
    }
    // a .c file with one opted-in function
    fs::path planted() { return put("planted.c", "// prism: run\nint planted(void) { return 0; }\n"); }
    std::string log() const {
        std::ifstream in(dir / "log");
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

prism::Config exec_cfg() {
    auto cfg = prism::default_config();
    cfg.allow_exec = true;
    cfg.timeout = 20;
    return cfg;
}

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string{} : it->second;
}

std::vector<prism::Finding> rows(const std::vector<prism::Finding>& out, const char* san, bool with_file = false) {
    std::vector<prism::Finding> r;
    for (auto& f : out)
        if (extra(f, "sanitizer") == san && (!with_file || !f.file.empty())) r.push_back(f);
    return r;
}

void no_proof(const std::vector<prism::Finding>& out) {
    for (auto& f : out) {
        CHECK(f.status != prism::laws::PROVED);
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }
}

}  // namespace

TEST_CASE("sanitize: no compiler on PATH is NOTRUN with an install hint") {
    FakeCc t;
    t.set("PATH", t.dir.string());  // an empty directory: no gcc, no clang
    auto none = prism::run_sanitize({}, exec_cfg());
    REQUIRE_FALSE(none.empty());
    auto out = prism::run_sanitize({t.planted()}, exec_cfg());
    REQUIRE(out.size() == 1);
    CHECK(out[0].stage == "sanitize");
    CHECK(out[0].status == prism::laws::NOTRUN);
    CHECK(out[0].message.find("not on PATH") != std::string::npos);
    CHECK_FALSE(extra(out[0], "install").empty());
    no_proof(out);
}

TEST_CASE("sanitize: a compiler without the sanitizers is NOTRUN for each, never CLEAN") {
    FakeCc t;  // FAKE_SAN empty: every -fsanitize build fails
    auto out = prism::run_sanitize({t.planted()}, exec_cfg());
    auto as = rows(out, "asan"), ub = rows(out, "ubsan"), ts = rows(out, "tsan");
    REQUIRE(as.size() == 1);
    REQUIRE(ub.size() == 1);
    REQUIRE(ts.size() == 1);
    CHECK(as[0].status == prism::laws::NOTRUN);
    CHECK(as[0].message.find("no ASan") != std::string::npos);
    CHECK(ub[0].status == prism::laws::NOTRUN);
    CHECK(ub[0].message.find("no UBSan") != std::string::npos);
    CHECK(ts[0].status == prism::laws::NOTRUN);
    for (auto& f : out) CHECK(f.status != prism::laws::CLEAN);
    no_proof(out);
}

TEST_CASE("sanitize: accepting -fsanitize without firing is not support") {
    FakeCc t;
    t.set("FAKE_SAN", "undefined address");
    t.set("FAKE_NOFIRE", "1");
    CHECK_FALSE(sd::probe(t.cc.string(), kUb));
    CHECK_FALSE(sd::probe(t.cc.string(), kAs));
    t.unset("FAKE_NOFIRE");
    CHECK(sd::probe(t.cc.string(), kUb));
    CHECK(sd::probe(t.cc.string(), kAs));
}

TEST_CASE("sanitize: supported UBSan / ASan with exit 0 is CLEAN, not a proof") {
    FakeCc t;
    t.set("FAKE_SAN", "undefined address");
    auto out = prism::run_sanitize({t.planted()}, exec_cfg());
    for (auto* san : {"ubsan", "asan"}) {
        auto r = rows(out, san, true);
        REQUIRE_MESSAGE(r.size() == 1, san);
        CHECK(r[0].status == prism::laws::CLEAN);
        CHECK(r[0].message.find("not a proof") != std::string::npos);
        CHECK(r[0].function == std::optional<std::string>("planted"));
    }
    CHECK(rows(out, "tsan")[0].status == prism::laws::NOTRUN);
    no_proof(out);
}

TEST_CASE("sanitize: a sanitizer abort is FAILED") {
    FakeCc t;
    t.set("FAKE_SAN", "undefined address");
    t.set("FAKE_RUN_undefined", "echo 'UndefinedBehaviorSanitizer: shift exponent' >&2; exit 1");
    t.set("FAKE_RUN_address", "echo 'ERROR: AddressSanitizer: heap-buffer-overflow' >&2; exit 1");
    auto out = prism::run_sanitize({t.planted()}, exec_cfg());
    auto ub = rows(out, "ubsan", true), as = rows(out, "asan", true);
    REQUIRE(ub.size() == 1);
    REQUIRE(as.size() == 1);
    CHECK(ub[0].status == prism::laws::FAILED);
    CHECK(as[0].status == prism::laws::FAILED);
    CHECK(as[0].evidence.find("AddressSanitizer") != std::string::npos);
}

TEST_CASE("sanitize: an unusable TSan runtime is NOTRUN, not FAILED") {
    const std::string mapping = "FATAL: ThreadSanitizer: unexpected memory mapping 0x7f00";
    CHECK(sd::runtime_unusable(mapping));
    CHECK_FALSE(sd::hit(mapping, -1));
    FakeCc t;
    t.set("FAKE_SAN", "thread");
    t.set("FAKE_RUN_thread", "echo '" + mapping + "' >&2; exit 66");
    auto out = prism::run_sanitize({t.planted()}, exec_cfg());
    auto ts = rows(out, "tsan", true);
    REQUIRE(ts.size() == 1);
    CHECK(ts[0].status == prism::laws::NOTRUN);
    for (auto& f : out) CHECK(f.status != prism::laws::FAILED);
    no_proof(out);
}

TEST_CASE("sanitize: sanitizer report text") {
    CHECK(sd::hit("ERROR: AddressSanitizer: heap-buffer-overflow", 1));
    CHECK(sd::hit("AddressSanitizer: heap-use-after-free", 1));
    CHECK(sd::hit("x.c:1: runtime error: shift exponent 40", 1));
    CHECK_FALSE(sd::hit("all good", 0));
    CHECK(sd::hit("", -6));  // killed by a signal
}

TEST_CASE("sanitize: MinGW is recognised by path and by -dumpmachine") {
    CHECK(sd::is_mingw_cc("C:\\mingw64\\bin\\gcc.exe"));
    CHECK(sd::is_mingw_cc("C:\\msys64\\mingw64\\bin\\gcc.exe"));
    FakeCc t("ucrt64");
    t.set("FAKE_TRIPLE", "x86_64-w64-mingw32");
    CHECK(sd::is_mingw_cc(t.cc.string()));
    t.set("FAKE_TRIPLE", "x86_64-linux-gnu");
    CHECK_FALSE(sd::is_mingw_cc(t.cc.string()));
}

TEST_CASE("sanitize: a -print-file-name echo is not a sanitizer runtime") {
    FakeCc t;
    CHECK_FALSE(sd::has_runtime_lib(t.cc.string(), kUb));
    CHECK_FALSE(sd::has_runtime_lib(t.cc.string(), kAs));
    CHECK_FALSE(sd::has_runtime_lib(t.cc.string(), kTs));
}

TEST_CASE("sanitize: MinGW without the runtime is NOTRUN and compiles nothing") {
    FakeCc t("mingw64");  // the path names MinGW; the runtime lookup echoes
    t.set("FAKE_SAN", "undefined address thread");
    CHECK_FALSE(sd::probe(t.cc.string(), kUb));
    CHECK_FALSE(sd::probe(t.cc.string(), kAs));
    CHECK(t.log().empty());  // the probe stopped before any compile
    auto out = prism::run_sanitize({t.planted()}, exec_cfg());
    REQUIRE_FALSE(out.empty());
    std::string msgs;
    for (auto& f : out) {
        CHECK(f.status == prism::laws::NOTRUN);
        msgs += f.message + " ";
    }
    CHECK(msgs.find("no ASan") != std::string::npos);
    CHECK(msgs.find("no UBSan") != std::string::npos);
    CHECK(msgs.find("no TSan") != std::string::npos);
    CHECK(t.log().empty());
    no_proof(out);
}

TEST_CASE("sanitize: a compiler that cannot start is NOTRUN, never ERROR or CLEAN") {
    FakeCc t;
    auto src = t.planted();
    auto [st, msg, ev] = sd::compile_and_run((t.dir / "vanished-gcc").string(), src, kUb, 8.0, "planted");
    CHECK(st == std::string(prism::laws::NOTRUN));
    CHECK(msg.find("failed to start") != std::string::npos);
    CHECK_FALSE(prism::laws::is_proof(st));
    // no opted-in function: NOTRUN with the opt-in rule
    auto [st2, msg2, ev2] = sd::compile_and_run(t.cc.string(), src, kUb, 8.0, std::nullopt);
    CHECK(st2 == std::string(prism::laws::NOTRUN));
    CHECK(msg2.find("prism: run") != std::string::npos);
}

TEST_CASE("sanitize: rows keep file order across sanitizers when runs are parallel") {
    FakeCc t;
    t.set("FAKE_SAN", "undefined address thread");
    std::vector<fs::path> files;
    for (int i = 0; i < 5; ++i)
        files.push_back(t.put("f" + std::to_string(i) + ".c",
                              "// prism: run\nint entry" + std::to_string(i) + "(void) { return 0; }\n"));
    auto cfg = exec_cfg();
    cfg.jobs = 4;
    auto out = prism::run_sanitize(files, cfg);
    REQUIRE(out.size() == 15);
    const char* order[] = {"asan", "ubsan", "tsan"};
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(extra(out[i], "sanitizer") == order[i / 5]);
        CHECK(out[i].file == files[i % 5].string());
        CHECK(out[i].status == prism::laws::CLEAN);
    }
}

TEST_CASE("sanitize: a live compiler gives sanitizer rows, never a proof") {
    auto cfg = exec_cfg();
    if (!cfg.which({"gcc", "clang"})) return;  // no toolchain: covered by the fake-compiler cases
    FakeCc t;  // only for the scratch dir and the restored environment
    t.set("PATH", t.saved["PATH"].value_or("/usr/bin:/bin"));
    auto src = t.put("live.c", "// prism: run\nint live(void) { return 0; }\n");
    auto out = prism::run_sanitize({src}, cfg);
    REQUIRE_FALSE(out.empty());
    for (auto& f : out) {
        CHECK(f.stage == "sanitize");
        auto s = extra(f, "sanitizer");
        CHECK((s == "asan" || s == "ubsan" || s == "tsan"));
        CHECK(f.status != prism::laws::PROVED);
    }
}

#endif  // _WIN32
