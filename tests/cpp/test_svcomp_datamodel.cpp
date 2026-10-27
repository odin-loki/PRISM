// Lock for `prism svcomp --data-model` (src/prism/cli_svcomp.cpp).
//
// The check is the upper() comparison in svcomp_main:
//   upper(*v) != "LP64" && upper(*v) != "ILP32"
// upper() is file-local. This calls svcomp_main; it does not spawn prism.
// A rejected value returns 2 before solve(), so it cannot select ILP32.
// docs/finish-notes/svcomp-datamodel.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/svcomp.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Capture {
    std::ostringstream out, err;
    std::streambuf* prev_out = nullptr;
    std::streambuf* prev_err = nullptr;
    Capture() : prev_out(std::cout.rdbuf(out.rdbuf())), prev_err(std::cerr.rdbuf(err.rdbuf())) {}
    ~Capture() {
        std::cout.rdbuf(prev_out);
        std::cerr.rdbuf(prev_err);
    }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
};

struct Call {
    int rc = 0;
    std::string out, err;
};

Call invoke(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& s : args) argv.push_back(s.data());
    Capture cap;
    Call c;
    c.rc = prism::svcomp::svcomp_main(static_cast<int>(argv.size()), argv.empty() ? nullptr : argv.data());
    c.out = cap.out.str();
    c.err = cap.err.str();
    return c;
}

const char* kExpects = "prism svcomp: --data-model expects LP64 or ILP32";
const char* kNeedTask = "prism svcomp: TASK and --prop are required";

}  // namespace

TEST_CASE("svcomp --data-model: omitted flag keeps the LP64 default") {
    CHECK(prism::svcomp::SolveOptions{}.data_model == "LP64");
    const Call c = invoke({});
    CHECK(c.rc == 2);
    CHECK(c.err.find(kExpects) == std::string::npos);
    CHECK(c.err.find(kNeedTask) != std::string::npos);
    CHECK(c.out.find("PRISM-SVCOMP-RESULT:") == std::string::npos);
}

TEST_CASE("svcomp --data-model: LP64 and ILP32 are accepted in any case") {
    for (const char* model : {"LP64", "lp64", "Lp64", "ILP32", "ilp32", "IlP32"}) {
        INFO(model);
        const Call spaced = invoke({"--data-model", model});
        CHECK(spaced.rc == 2);
        CHECK(spaced.err.find(kExpects) == std::string::npos);
        CHECK(spaced.err.find(kNeedTask) != std::string::npos);
        CHECK(spaced.out.find("PRISM-SVCOMP-RESULT:") == std::string::npos);

        const Call joined = invoke({std::string("--data-model=") + model});
        CHECK(joined.rc == 2);
        CHECK(joined.err.find(kExpects) == std::string::npos);
        CHECK(joined.err.find(kNeedTask) != std::string::npos);
        CHECK(joined.out.find("PRISM-SVCOMP-RESULT:") == std::string::npos);
    }
}

TEST_CASE("svcomp --data-model: ILP64, LP32, and empty exit and do not run") {
    const std::vector<std::vector<std::string>> rejected = {
        {"--data-model", "ILP64", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model", "ilp64", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model", "LP32", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model", "lp32", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model", "", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model=", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model=ILP64", "--prop", "no-overflow.prp", "task.c"},
        {"--data-model=LP32", "--prop", "no-overflow.prp", "task.c"},
    };
    for (const auto& args : rejected) {
        std::string shown;
        for (const auto& a : args) {
            if (!shown.empty()) shown.push_back(' ');
            shown += a.empty() ? std::string("\"\"") : a;
        }
        INFO(shown);
        const Call c = invoke(args);
        CHECK(c.rc == 2);
        CHECK(c.err.find(kExpects) != std::string::npos);
        CHECK(c.err.find(kNeedTask) == std::string::npos);
        CHECK(c.out.find("PRISM-SVCOMP-RESULT:") == std::string::npos);
    }
}
