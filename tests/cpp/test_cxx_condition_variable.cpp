// The <condition_variable> model is one bool in the object. notify_one and
// notify_all set that flag and do not run a waiter. A default object has the
// flag false. wait returns at once (a spurious wakeup the standard permits);
// wait(lock, pred) assumes pred() on return.
//
// Included by path so prism_tests does not put the model directory on the
// include path for every TU. `#include <condition_variable>` here would be
// the platform header.

#include <doctest/doctest.h>

#include "prose_text.hpp"
#include "prism/pir.hpp"

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/condition_variable"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "condition_variable"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of the function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// `name` as its own identifier, then '('. wait_for does not define wait.
bool defines_call(std::string_view src, std::string_view name) {
    for (std::size_t i = 0; i + name.size() <= src.size(); ++i) {
        if (src.compare(i, name.size(), name) != 0) continue;
        if (i > 0 && is_ident(src[i - 1])) continue;
        std::size_t j = i + name.size();
        if (j < src.size() && is_ident(src[j])) continue;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) ++j;
        if (j < src.size() && src[j] == '(') return true;
    }
    return false;
}

}  // namespace

TEST_CASE("cxx condition_variable model: waits explore thread interleavings through the scheduler") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    CHECK(src.find("#ifndef _GLIBCXX_CONDITION_VARIABLE") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_CONDITION_VARIABLE 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    // the scheduler, included by a quoted path so it resolves next to the
    // model (no libstdc++ file of that name exists)
    CHECK(src.find("#include \"bits/prism_sched.h\"") != std::string::npos);
    CHECK(src.find("not a concurrency proof") == std::string::npos);
    CHECK(src.find("spurious wakeup") != std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("class condition_variable_any") != std::string::npos);
    CHECK(src.find("condition_variable(const condition_variable&) = delete;") != std::string::npos);

    // wait(l, pred): release, re-check, must switch, relock - never assume pred
    const std::string wp = body_of(src, "inline void __wait_pred(");
    REQUIRE_FALSE(wp.empty());
    CHECK(wp.find("__l.unlock()") != std::string::npos);
    CHECK(wp.find("__must_switch") != std::string::npos);
    CHECK(wp.find("__l.lock()") != std::string::npos);
    CHECK(wp.find("__prism_assume") == std::string::npos);
    CHECK(wp.find("__l.unlock()") < wp.find("__must_switch"));
    CHECK(wp.find("__must_switch") < wp.find("__l.lock()"));
    // plain wait: an optional switch (spurious wakeup when none runs)
    const std::string wo = body_of(src, "inline void __wait_once(");
    CHECK(wo.find("__maybe_switch") != std::string::npos);
    // timed wait with nothing queued times out
    const std::string wt = body_of(src, "inline bool __wait_timed(");
    CHECK(wt.find("__prism_sched::__n == 0") != std::string::npos);
    // notify is a switch point
    const std::string one = body_of(src, "void notify_one() noexcept");
    CHECK(one.find("__maybe_switch") != std::string::npos);
    CHECK(defines_call(src, "wait"));
    CHECK(defines_call(src, "wait_for"));
    CHECK(defines_call(src, "wait_until"));
    CHECK_FALSE(defines_call(src, "notify_all_at_thread_exit"));
}

TEST_CASE("cxx scheduler model: deadlock check, preemption bound, prune of unrepresentable schedules") {
    const std::string s = prism_test::unwrap_prose(slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "bits" / "prism_sched.h"));
    CHECK(s.find("\"CXX-DEADLOCK\"") != std::string::npos);
    CHECK(s.find("#define PRISM_SCHED_PREEMPTIONS 2") != std::string::npos);
    const std::string ms = body_of(s, "inline void __maybe_switch()");
    CHECK(ms.find("__budget == 0") != std::string::npos);
    CHECK(ms.find("--__budget") != std::string::npos);
    const std::string mu = body_of(s, "inline void __must_switch(");
    CHECK(mu.find("__prism_assume(0)") != std::string::npos);
    CHECK(mu.find("__prism_assume(0)") < mu.find("CXX-DEADLOCK"));
    const std::string th = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "thread");
    CHECK(th.find("\"CXX-THREAD-JOINABLE\"") != std::string::npos);
    CHECK(th.find("__maybe_start()") != std::string::npos);
    const std::string at = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "atomic");
    CHECK(at.find("__maybe_switch") != std::string::npos);
    // translate.cpp maps the model classes onto the taxonomy
    const std::string tr = slurp(repo() / "src" / "prism" / "pir" / "translate.cpp");
    CHECK(tr.find("\"CXX-DEADLOCK\") cls = std::string(\"CONC-DEADLOCK\")") != std::string::npos);
    CHECK(tr.find("\"CXX-THREAD-JOINABLE\") cls = std::string(\"CXX-TERMINATE\")") != std::string::npos);
}

TEST_CASE("cxx models: source_imports_std") {
    CHECK(prism::pir::source_imports_std("import std;\nint main(){}"));
    CHECK(prism::pir::source_imports_std("  import   std ;"));
    CHECK(prism::pir::source_imports_std("export import std.compat;"));
    CHECK_FALSE(prism::pir::source_imports_std("// import std;\n"));
    CHECK_FALSE(prism::pir::source_imports_std("/* import std; */"));
    CHECK_FALSE(prism::pir::source_imports_std("const char* s = \"import std;\";"));
    CHECK_FALSE(prism::pir::source_imports_std("import stdx;"));
    CHECK_FALSE(prism::pir::source_imports_std("int x; import std;"));
    CHECK_FALSE(prism::pir::source_imports_std("#include <vector>"));
}
