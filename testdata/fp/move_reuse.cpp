// Correct C++ after std::move: the moved-from name is re-declared, assigned a
// new value, or only moved on a path that leaves. Reduced from
// src/prism/adapters.cpp run_cppcheck_unstamped, pir/memory.cpp shallow_vs,
// pir/stage.cpp run_unit, ai/proof_repair.cpp and stages/fuse.cpp.
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Row {
    std::string text;
    std::vector<int> kinds;
};

std::vector<std::string> redeclared(bool started, const std::string& err) {
    std::vector<std::string> out;
    if (!started) {
        std::string msg = "failed to start";
        out.push_back(std::move(msg));
        return out;
    }
    auto msg = err.substr(0, 10);
    if (msg.empty()) msg = "exit";
    out.push_back(msg);
    return out;
}

std::string reassigned(std::string a) {
    std::string keep = std::move(a);
    a = "fresh";
    return keep + a.substr(0, 1);
}

int loop_locals(const std::vector<int>& xs, std::vector<Row>& rows) {
    int n = 0;
    for (int x : xs) {
        Row r;
        r.text = std::to_string(x);
        if (x < 0) {
            rows.push_back(std::move(r));
            continue;
        }
        r.kinds.push_back(x);
        n += static_cast<int>(r.kinds.size());
    }
    return n;
}

std::optional<Row> first_ok(const std::vector<int>& xs) {
    std::optional<Row> found;
    for (int x : xs) {
        Row r;
        r.text = std::to_string(x);
        if (x > 0) {
            found = std::move(r);
            break;
        }
        r.kinds.clear();
    }
    return found;
}

int other_object(Row a, const Row& e) {
    Row b = std::move(a);
    return static_cast<int>(e.kinds.size() + b.kinds.size());
}

std::string lambda_param(const std::string& raw) {
    auto make = [&](std::string text) {
        Row r;
        r.text = std::move(text);
        return r;
    };
    const std::string text = raw + "!";
    return make(raw).text + text.substr(0, 1);
}
