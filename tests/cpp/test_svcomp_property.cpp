// Lock for prism::svcomp::parse_property and supported_property
// (src/prism/svcomp/property.cpp).
//
// An unknown property file parses as "unsupported". An unknown property
// id is outside no-overflow, unreach-call and valid-memsafety, so decide
// answers "unknown" and solve answers "unknown" (a missing file is
// "error"). valid-memsafety stays its own id. docs/finish-notes/svcomp-property.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/svcomp.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using prism::svcomp::decide;
using prism::svcomp::json;
using prism::svcomp::parse_property;
using prism::svcomp::solve;
using prism::svcomp::SolveOptions;
using prism::svcomp::supported_property;
namespace laws = prism::laws;

namespace {

fs::path repo_root() {
    fs::path from_file = fs::path(__FILE__).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (fs::is_directory(from_file / "testdata" / "conformance" / "sv-comp", ec)) return from_file;
    for (fs::path d = fs::current_path(); !d.empty(); d = d.parent_path()) {
        if (fs::is_directory(d / "testdata" / "conformance" / "sv-comp", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return from_file;
}

fs::path props_dir() { return repo_root() / "testdata" / "conformance" / "sv-comp" / "properties"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
#ifdef _WIN32
        const long pid = static_cast<long>(_getpid());
#else
        const long pid = static_cast<long>(::getpid());
#endif
        path = fs::temp_directory_path() /
               ("prism_svcomp_property_" + std::to_string(pid) + "_" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        std::ofstream(path / name, std::ios::binary) << text;
        return path / name;
    }
};

json row(std::string_view status, std::string_view cls = "", std::string_view message = "") {
    return json{{"function", "main"},
                {"status", std::string(status)},
                {"cls", std::string(cls)},
                {"message", std::string(message)}};
}

json report(std::initializer_list<std::pair<std::string, std::vector<json>>> stages) {
    json rep = {{"stages", json::array()}};
    for (const auto& [name, findings] : stages) {
        json rows = json::array();
        for (const auto& f : findings) rows.push_back(f);
        rep["stages"].push_back({{"name", name}, {"status", "ok"}, {"findings", rows}});
    }
    return rep;
}

json confirmed(const json&) {
    return json{{"replay", "replayed"}, {"detail", "UBSan: signed integer overflow"}};
}

json overflow_cex() {
    json f = row(laws::FAILED, "INT-SIGNED-OVF", "signed overflow");
    f["counterexample"] = "n=2147483647";
    f["extra"] = json{{"nondet", "n=2147483647"}};
    return f;
}

void check_unknown_verdict(const std::string& answer) {
    CHECK(answer == "unknown");
    CHECK(answer != "true");
    CHECK(answer.find("false") != 0);
}

// Upstream property texts that are outside the supported set. A hit on
// "overflow" or "reach_error" inside a different formula stays unsupported.
const char* kUnknownFiles[] = {
    "",
    "\n",
    "no-overflow",
    "valid-memsafety",
    "overflow",
    "CHECK( init(main()), LTL(F end) )\n",
    "CHECK( init(main()), LTL(G def-behavior) )\n",
    "CHECK( init(main()), LTL(G ! data-race) )\n",
    "CHECK( init(main()), LTL(G valid-memcleanup) )\n",
    "CHECK( init(main()), LTL(G valid-memsafety) )\n",
    "CHECK( init(main()), LTL(G ! unsigned-overflow) )\n",
    "CHECK( init(main()), LTL(G ! call(overflow())) )\n",
    "CHECK( init(main()), LTL(G ! call(__VERIFIER_error())) )\n",
    "COVER( init(main()), FQL(COVER EDGES(@DECISIONEDGE)) )\n",
    "COVER( init(main()), FQL(COVER EDGES(@CALL(reach_error))) )\n",
    "CHECK( init(main()), LTL(G ! overflow) )\nCHECK( init(main()), LTL(G ! call(reach_error())) )\n",
    "CHECK( init(main()), LTL(G ! overflow) )\nCHECK( init(main()), LTL(G valid-free) )\n",
};

const char* kUnknownIds[] = {
    "",
    "unsupported",
    "termination",
    "def-behavior",
    "no-data-race",
    "valid-memcleanup",
    "valid-deref",
    "valid-free",
    "valid-memtrack",
    "valid-memsafety.prp",
    "no-overflow.prp",
    "NO-OVERFLOW",
    "overflow",
    "no-overflow ",
};

}  // namespace

TEST_CASE("svcomp property: an unknown file is unsupported, and valid-memsafety is its own id") {
    CHECK(parse_property(slurp(props_dir() / "no-overflow.prp")) == "no-overflow");
    CHECK(parse_property(slurp(props_dir() / "unreach-call.prp")) == "unreach-call");
    const std::string mem = parse_property(slurp(props_dir() / "valid-memsafety.prp"));
    CHECK(mem == "valid-memsafety");
    CHECK(mem != "no-overflow");

    CHECK(parse_property("CHECK( init(main()), LTL(G ! overflow) )") == "no-overflow");
    CHECK(parse_property("CHECK( init(main()), LTL(G ! call(reach_error())) )") == "unreach-call");
    CHECK(parse_property("CHECK( init(main()), LTL(G valid-free) )\n"
                         "CHECK( init(main()), LTL(G valid-deref) )\n"
                         "CHECK( init(main()), LTL(G valid-memtrack) )\n") == "valid-memsafety");

    CHECK(supported_property("no-overflow"));
    CHECK(supported_property("unreach-call"));
    CHECK(supported_property("valid-memsafety"));

    for (const char* text : kUnknownFiles) {
        INFO(text);
        const std::string got = parse_property(text);
        CHECK(got == "unsupported");
        CHECK(got != "no-overflow");
        CHECK(got != "valid-memsafety");
        CHECK_FALSE(supported_property(got));
    }
    for (const char* id : kUnknownIds) {
        INFO(id);
        CHECK_FALSE(supported_property(id));
    }
}

TEST_CASE("svcomp property: an unknown id stays unknown, and valid-memsafety is not no-overflow") {
    auto proved = report({{"bmc", {row(laws::PROVED, "", "main proved")}}});
    auto ovf = report({{"bmc", {overflow_cex()}}});

    CHECK(decide(proved, "no-overflow", confirmed).answer == "true");
    check_unknown_verdict(decide(proved, "valid-memsafety", confirmed).answer);
    CHECK(decide(ovf, "no-overflow", confirmed).answer == "false(no-overflow)");
    check_unknown_verdict(decide(ovf, "valid-memsafety", confirmed).answer);

    for (const char* id : kUnknownIds) {
        INFO(id);
        auto proved_d = decide(proved, id, confirmed);
        check_unknown_verdict(proved_d.answer);
        CHECK(proved_d.reason == std::string("property ") + id + " is not supported");
        auto failed_d = decide(ovf, id, confirmed);
        check_unknown_verdict(failed_d.answer);
        CHECK(failed_d.reason == std::string("property ") + id + " is not supported");
    }
}

TEST_CASE("svcomp property: an unknown property file verdict is unknown, a missing file is error") {
    TempDir d;
    auto task = d.write("t.c", "int main(void) { return 0; }\n");
    SolveOptions opt;
    opt.allow_exec = false;
    opt.out = d.path / "out";
    opt.witness = d.path / "witness.yml";

    auto missing = solve(task, d.path / "missing.prp", opt);
    CHECK(missing.decision.answer == "error");
    CHECK(missing.decision.answer != "true");
    CHECK(missing.decision.answer.find("false") != 0);
    CHECK(missing.decision.reason.find("cannot read property file") != std::string::npos);

    for (const char* text : kUnknownFiles) {
        INFO(text);
        auto prp = d.write("bad.prp", text);
        auto oc = solve(task, prp, opt);
        check_unknown_verdict(oc.decision.answer);
        CHECK(oc.decision.reason == "unsupported property file bad.prp");
    }
}
