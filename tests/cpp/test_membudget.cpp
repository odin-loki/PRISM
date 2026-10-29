// Doctests: the process-wide memory budget (prism/membudget.hpp, Law 7).
// Over the budget a function is a reported UNKNOWN, never a crash, and the
// functions checked after it still get their verdicts (nothing sticky).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/membudget.hpp"
#include "prism/pir.hpp"

#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace mb = prism::membudget;

const char* kIr = R"IR(
define i32 @ok(i32 %x) {
entry:
  %r = and i32 %x, 1
  ret i32 %r
}
define i32 @divz(i32 %x) {
entry:
  %r = sdiv i32 100, %x
  ret i32 %r
}
)IR";

pp::Translation tr(const std::string& fn) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module(kIr));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    return pp::translate(keep.back(), *f, {});
}

struct Restore {
    ~Restore() { mb::set_budget_mb_for_test(0); }
};

}  // namespace

TEST_CASE("membudget: a budget and the resident set are measured") {
#ifdef __linux__
    CHECK(mb::rss_mb() > 0);
    if (mb::budget_mb() > 0) CHECK_FALSE(mb::budget_source().empty());
#endif
    mb::set_budget_mb_for_test(1000);
    Restore r;
    CHECK(mb::per_job_mb(4) == 250);
    CHECK(mb::per_job_mb(0) == 1000);
}

TEST_CASE("membudget: over the budget a function is UNKNOWN with the reason, later functions are checked") {
#ifndef __linux__
    MESSAGE("SKIP: RSS is read from /proc");
    return;
#endif
    auto ok = tr("ok"), divz = tr("divz");
    REQUIRE(ok.fn);
    REQUIRE(divz.fn);
    Restore r;
    mb::set_budget_mb_for_test(1);  // every process is over 1 MB
    for (auto* t : {&ok, &divz}) {
        auto v = pp::check_function(*t->fn, 4, 30.0);
        CHECK(v.status == prism::laws::UNKNOWN);
        CHECK(v.message.find("memory budget") != std::string::npos);
        CHECK(v.status != prism::laws::PROVED);
    }
    mb::set_budget_mb_for_test(0);  // back under: not sticky
    CHECK(pp::check_function(*ok.fn, 4, 30.0).status == prism::laws::PROVED);
    CHECK(pp::check_function(*divz.fn, 4, 30.0).status == prism::laws::FAILED);
    CHECK_FALSE(pp::z3_hard_oom());
}
