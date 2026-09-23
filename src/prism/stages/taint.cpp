// Stage taint: source-to-sink taint through assignments and copies (run_taint).
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
std::vector<std::string> taint_split_args(const std::string& argtext) {
    std::vector<std::string> args;
    int depth = 0;
    std::string cur;
    for (char ch : argtext) {
        if (ch == '(') {
            ++depth;
            cur.push_back(ch);
        } else if (ch == ')') {
            depth = std::max(0, depth - 1);
            cur.push_back(ch);
        } else if (ch == ',' && depth == 0) {
            args.push_back(strip(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    auto tail = strip(cur);
    if (!tail.empty()) args.push_back(tail);
    return args;
}

std::vector<int> sink_arg_indices(const std::string& name) {
    if (name == "system" || name == "popen" || name == "execl" || name == "execv" || name == "printf")
        return {0};
    if (name == "sprintf" || name == "fprintf") return {1};
    if (name == "strcpy" || name == "memcpy" || name == "strcat" || name == "strncat") return {0, 1};
    return {0};
}

bool arg_is_tainted(const std::string& arg, const std::set<std::string>& tainted) {
    static Regex str_re(R"("([^"\\]|\\.)*")");
    std::string stripped;
    std::size_t i = 0;
    for (auto& m : str_re.finditer(arg)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        stripped.append(arg, i, a - i);
        i = b;
    }
    stripped.append(arg, i, std::string::npos);
    static Regex ident("\\b([A-Za-z_]\\w*)\\b");
    for (auto& m : ident.finditer(stripped))
        if (tainted.contains(m.group(1))) return true;
    return false;
}

std::vector<std::pair<int, std::string>> iter_statements(const std::string& body, int base_line) {
    std::vector<std::string> lines;
    {
        std::istringstream ss(body);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    std::vector<std::pair<int, std::string>> out;
    std::vector<std::string> buf;
    int start = 0;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (buf.empty()) start = i;
        buf.push_back(lines[static_cast<std::size_t>(i)]);
        auto raw = lines[static_cast<std::size_t>(i)];
        if (raw.find(';') != std::string::npos || strip(raw).ends_with("}")) {
            auto stmt_s = strip(join_sv(buf, " "));
            if (!stmt_s.empty()) out.emplace_back(base_line + start + 1, stmt_s);
            buf.clear();
        }
    }
    auto tail = strip(join_sv(buf, " "));
    if (!tail.empty()) out.emplace_back(base_line + start + 1, tail);
    return out;
}

void mark_source_taint(const std::string& stmt_s, std::set<std::string>& tainted) {
    static const std::vector<std::string> sources{"getenv", "fgets", "gets", "scanf", "recv", "read", "fread", "recvfrom"};
    static Regex asg("(?P<lhs>[A-Za-z_]\\w*(?:\\s*\\[[^\\]]*\\])?)\\s*=\\s*(?P<rhs>[^;]+)");
    for (auto& m : asg.finditer(stmt_s)) {
        auto lhs = m.named("lhs");
        auto br = lhs.find('[');
        if (br != std::string::npos) lhs = strip(lhs.substr(0, br));
        else lhs = strip(lhs);
        auto rhs = m.named("rhs");
        for (auto& src : sources)
            if (re_search("\\b" + src + "\\s*\\(", rhs)) tainted.insert(lhs);
    }
    for (auto& src : sources) {
        Regex call("\\b" + src + "\\s*\\((?P<args>[^)]*)\\)");
        for (auto& m : call.finditer(stmt_s)) {
            auto args = taint_split_args(m.named("args"));
            if (args.empty()) continue;
            auto take = [](std::string a) {
                a = strip(a);
                while (!a.empty() && (a.front() == '&' || a.front() == '*')) a.erase(a.begin());
                auto br = a.find('[');
                if (br != std::string::npos) a = a.substr(0, br);
                return strip(a);
            };
            if (src == "read" && args.size() >= 2) tainted.insert(take(args[1]));
            else if (src == "recv" && args.size() >= 2) tainted.insert(take(args[1]));
            else if (src == "recvfrom" && args.size() >= 2) tainted.insert(take(args[1]));
            else if (src == "fread") tainted.insert(take(args[0]));
            else if (src == "fgets" || src == "gets") tainted.insert(take(args[0]));
            else if (src == "scanf") {
                for (std::size_t i = 1; i < args.size(); ++i) tainted.insert(take(args[i]));
            }
        }
    }
}

void mark_copy_taint(const std::string& stmt_s, std::set<std::string>& tainted) {
    static Regex re("(?P<dst>[A-Za-z_]\\w*)\\s*=\\s*(?:\\([^)]*\\)\\s*)*(?P<src>[A-Za-z_]\\w*)\\s*(?:;|$)");
    auto m = re.search_match(stmt_s);
    if (!m) return;
    if (tainted.contains(m->named("src"))) tainted.insert(m->named("dst"));
}

std::optional<Finding> analyze_taint(const FunctionInfo& fn) {
    std::set<std::string> tainted;
    for (auto& [t, name] : fn.params)
        if (name == "argv" || name == "envp") tainted.insert(name);
    for (auto& [line_no, stmt_s] : iter_statements(fn.body, fn.line)) {
        mark_source_taint(stmt_s, tainted);
        mark_copy_taint(stmt_s, tainted);
        static Regex sink(
            "\\b(?P<name>system|popen|execl|execv|strcpy|sprintf|memcpy|strcat|strncat)\\s*\\((?P<args>[^)]*)\\)");
        for (auto& m : sink.finditer(stmt_s)) {
            auto name = m.named("name");
            auto args = taint_split_args(m.named("args"));
            for (int idx : sink_arg_indices(name)) {
                if (idx >= static_cast<int>(args.size())) continue;
                if (arg_is_tainted(args[static_cast<std::size_t>(idx)], tainted)) {
                    auto f = make_find("taint", laws::FAILED, fn, "TAINT-SINK",
                                       "tainted data reaches " + name + "()", laws::STRENGTH_FINDS);
                    f.line = line_no;
                    f.evidence = strip(stmt_s);
                    return f;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::vector<Finding> run_taint(const std::vector<FunctionInfo>& functions) {
    std::vector<Finding> out;
    for (auto& fn : functions)
        if (auto hit = analyze_taint(fn)) out.push_back(*hit);
    return out;
}

}  // namespace prism
