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

bool taint_id_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool taint_plain_ident(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s.front())) || s.front() == '_')) return false;
    for (char c : s)
        if (!taint_id_char(c)) return false;
    return true;
}

std::string taint_compact(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) o.push_back(c);
    return o;
}

// A non-identifier label (`*p`, `*&buf`) matches when that expression occurs
// in the sink argument. Whitespace is ignored; an identifier character on
// either side blocks a partial overlap (`*p` is not `*ptr` or `x*p`).
bool taint_expr_in(const std::string& arg, const std::string& label) {
    if (taint_plain_ident(label)) return false;
    bool named = false;
    for (char c : label)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') named = true;
    if (!named) return false;
    auto hay = taint_compact(arg);
    auto needle = taint_compact(label);
    if (needle.empty() || hay.size() < needle.size()) return false;
    std::size_t from = 0;
    while (from + needle.size() <= hay.size()) {
        auto pos = hay.find(needle, from);
        if (pos == std::string::npos) return false;
        bool left = pos == 0 || !taint_id_char(hay[pos - 1]);
        auto end = pos + needle.size();
        bool right = end == hay.size() || !(taint_id_char(needle.back()) && taint_id_char(hay[end]));
        if (left && right) return true;
        from = pos + 1;
    }
    return false;
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
    for (auto& name : tainted)
        if (taint_expr_in(stripped, name)) return true;
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
            // '&' is the address of the buffer on every source (`fread(&buf)`,
            // `read(fd, &buf, n)`). '*' is stripped only for scanf
            // (`scanf("%s", *p)` names p). On the other sources a leading '*'
            // is the pointer expression and stays, so `fread(*p)` does not
            // label the different name p. A '[' subscript is cut after that,
            // then surrounding whitespace, so `& buf` and `buf[i]` still name buf.
            auto take = [](std::string a, bool scanf_star) {
                a = strip(a);
                while (!a.empty() && (a.front() == '&' || (scanf_star && a.front() == '*')))
                    a.erase(a.begin());
                auto br = a.find('[');
                if (br != std::string::npos) a = a.substr(0, br);
                return strip(a);
            };
            if (src == "read" && args.size() >= 2) tainted.insert(take(args[1], false));
            else if (src == "recv" && args.size() >= 2) tainted.insert(take(args[1], false));
            else if (src == "recvfrom" && args.size() >= 2) tainted.insert(take(args[1], false));
            else if (src == "fread") tainted.insert(take(args[0], false));
            else if (src == "fgets" || src == "gets") tainted.insert(take(args[0], false));
            else if (src == "scanf") {
                for (std::size_t i = 1; i < args.size(); ++i) tainted.insert(take(args[i], true));
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
