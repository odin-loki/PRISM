#include "prism/checkers.hpp"
#include "prism/threads.hpp"

#include "prism/cparse.hpp"
#include "prism/laws.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <ranges>
#include <sstream>

namespace prism {

void lint_add(std::vector<Finding>& out, std::string_view rel, std::optional<std::string> fn,
              int line, std::string_view cls, std::string_view msg,
              const std::vector<std::string>& lines) {
    Finding f;
    f.stage = "lints";
    f.status = std::string(laws::FAILED);
    f.file = std::string(rel);
    f.function = std::move(fn);
    f.line = line;
    f.cls = std::string(cls);
    f.message = std::string(msg);
    f.strength = std::string(laws::STRENGTH_FINDS);
    if (line > 0 && static_cast<std::size_t>(line) <= lines.size()) f.evidence = lines[static_cast<std::size_t>(line) - 1];
    out.push_back(std::move(f));
}

void lint_discarded(const std::vector<FunctionInfo>& funcs,
                    const std::vector<std::string>& lines,
                    std::string_view rel,
                    const Regex& re,
                    std::string_view cls,
                    std::string_view msg,
                    std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        std::size_t i = 0;
        for (auto ln : fn.body | std::views::split('\n')) {
            std::string s(ln.begin(), ln.end());
            if (re.match_line(s) || re.search(s)) {
                int line = start + static_cast<int>(i);
                lint_add(out, rel, fn.name, line, cls, msg, lines);
            }
            ++i;
        }
    }
}

std::vector<Finding> run_lints(const std::vector<std::filesystem::path>& paths,
                               const std::filesystem::path& root, int jobs) {
    std::vector<Finding> out;
    std::mutex mu;
    parallel_for(jobs, paths, [&](std::size_t, const std::filesystem::path& p) {
        std::string rel;
        std::error_code ec;
        if (std::filesystem::is_directory(root)) {
            auto r = std::filesystem::relative(p, root, ec);
            rel = ec ? p.filename().string() : r.generic_string();
        } else {
            rel = p.filename().string();
        }
        std::ifstream in(p, std::ios::binary);
        if (!in) return;
        std::ostringstream ss;
        ss << in.rdbuf();
        auto text = ss.str();
        auto stripped = strip_comments_keep_lines(text, true);
        std::vector<std::string> lines;
        std::string line;
        std::istringstream ls(stripped);
        while (std::getline(ls, line)) lines.push_back(line);
        // cparse bodies keep string literals. Pattern checks read code, so
        // they get bodies with literal contents blanked (quotes, lengths and
        // lines kept): `puts("never call gets(b)")` is not a gets() call.
        // Only checkers that read literal bytes get `raw_funcs`.
        auto raw_funcs = extract_functions(p, rel);
        // Pad each body so body line i is file line span.first + i: a body
        // starts after its `{`, and with the brace on a line below the head
        // body-walking checkers reported one line early.
        for (auto& f : raw_funcs) {
            int head = f.span.first - 1;
            int end = std::min(static_cast<int>(lines.size()), f.span.second);
            for (int j = std::max(head, 0); j < end; ++j) {
                if (lines[static_cast<std::size_t>(j)].find('{') == std::string::npos) continue;
                if (j > head) f.body = std::string(static_cast<std::size_t>(j - head), '\n') + f.body;
                break;
            }
        }
        auto funcs = raw_funcs;
        for (auto& f : funcs)
            if (f.body.find('"') != std::string::npos) f.body = strip_comments_keep_lines(f.body, true);
        std::vector<Finding> local;
        checkers_core(lines, rel, funcs, raw_funcs, p, text, local);
        auto ext = p.extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".cpp" && ext != ".cc" && ext != ".cxx")
            checkers_api(lines, rel, funcs, local);
        else
            checkers_cxx(lines, rel, funcs, stripped, local);
        std::lock_guard<std::mutex> lock(mu);
        out.insert(out.end(), local.begin(), local.end());
    });
    return out;
}

}  // namespace prism
