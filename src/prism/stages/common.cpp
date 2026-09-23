// Helpers shared by the stages (declared in common.hpp): text and regex
// helpers, findings, source lookup, argument decoding and seeds, bmc_one and
// the ACSL / comment contract parser.
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace stages_detail {
std::string strip(std::string s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lstrip(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string join_sv(const std::vector<std::string>& v, std::string_view sep) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += sep;
        o += v[i];
    }
    return o;
}

bool starts_kw(std::string_view text, std::string_view kw) {
    if (!text.starts_with(kw)) return false;
    if (text.size() == kw.size()) return true;
    unsigned char c = static_cast<unsigned char>(text[kw.size()]);
    return !(std::isalnum(c) || c == '_');
}

std::optional<Match> match_at(const Regex& re, std::string_view s) {
    auto m = re.search_match(s, 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}

bool fullmatch(const Regex& re, std::string_view s) {
    auto m = match_at(re, s);
    return m && static_cast<std::size_t>(m->spans[0].second) == s.size();
}

int32_t i32(int64_t x) {
    auto u = static_cast<uint32_t>(static_cast<uint64_t>(x) & 0xFFFFFFFFu);
    if (u >= 0x80000000u) return static_cast<int32_t>(static_cast<int64_t>(u) - 0x100000000LL);
    return static_cast<int32_t>(u);
}

bool truth(int64_t v) { return i32(v) != 0; }

Finding make_find(std::string stage, std::string_view status, const FunctionInfo& fn, std::string cls,
                  std::string msg, std::string_view strength) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(status);
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.cls = std::move(cls);
    f.message = std::move(msg);
    f.strength = std::string(strength);
    return f;
}

Finding nr(std::string stage, std::string msg, std::string strength) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(laws::NOTRUN);
    f.message = std::move(msg);
    f.strength = std::move(strength);
    return f;
}

std::vector<std::string> split_comma(const std::string& s) {
    std::vector<std::string> parts;
    int pdepth = 0, bdepth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(') ++pdepth;
        else if (ch == ')') --pdepth;
        else if (ch == '[') ++bdepth;
        else if (ch == ']') --bdepth;
        if (ch == ',' && pdepth == 0 && bdepth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::pair<std::string, std::string> paren(std::string text) {
    text = lstrip(std::move(text));
    if (!text.starts_with("(")) throw ParseFail("expected (");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced (");
}

std::pair<std::string, std::string> brace(std::string text) {
    text = lstrip(std::move(text));
    if (!text.starts_with("{")) throw ParseFail("expected {");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced {");
}

std::pair<std::string, std::string> stmt(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == '{' && depth == 0) break;
        else if (ch == ';' && depth == 0) return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<std::size_t>(80, text.size())));
}

std::string read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<fs::path> locate_source(const FunctionInfo& fn) {
    if (fn.file.empty()) return std::nullopt;
    fs::path p = fn.file;
    if (fs::is_regular_file(p)) return p;
    auto cwd = fs::current_path() / p;
    if (fs::is_regular_file(cwd)) return cwd;
    // In a pipeline run fn.file is relative to the scanned root, which need
    // not be the cwd: without this, `// requires:` specs were silently not
    // found (contracts/wp/harness saw no spec) for `prism /some/tree`.
    if (auto* c = ai::session_config()) {
        std::error_code ec;
        auto base = fs::is_directory(c->root, ec) ? c->root : c->root.parent_path();
        if (fs::is_regular_file(base / p, ec)) return base / p;
    }
    fs::path name = p.filename();
    fs::path walk = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        auto td = walk / "testdata" / name;
        if (fs::is_regular_file(td)) return td;
        if (!walk.has_parent_path() || walk == walk.parent_path()) break;
        walk = walk.parent_path();
    }
    return std::nullopt;
}

std::string read_fn_source(const FunctionInfo& fn) {
    auto p = locate_source(fn);
    if (!p) return {};
    try {
        return read_text_file(*p);
    } catch (...) {
        return {};
    }
}

std::optional<fs::path> which_cc() {
    Config cfg;
    return cfg.which({"gcc", "clang", "cl"});
}

int param_nbytes(const std::vector<std::pair<std::string, std::string>>& params) {
    int n = 0;
    for (auto& [typ, _] : params) {
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        auto it = kCTypeSize.find(key);
        n += it == kCTypeSize.end() ? 4 : it->second;
    }
    return n ? n : 1;
}

std::map<std::string, int> decode_args(const FunctionInfo& fn, const std::vector<uint8_t>& data) {
    std::map<std::string, int> args;
    std::size_t off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::vector<uint8_t> chunk(static_cast<std::size_t>(sz), 0);
        for (int i = 0; i < sz && off + static_cast<std::size_t>(i) < data.size(); ++i)
            chunk[static_cast<std::size_t>(i)] = data[off + static_cast<std::size_t>(i)];
        bool uns = key.starts_with("uint") || key.starts_with("unsigned") || key == "size_t" || key == "uintptr_t";
        int64_t raw = 0;
        for (int i = sz - 1; i >= 0; --i) raw = (raw << 8) | chunk[static_cast<std::size_t>(i)];
        if (!uns && sz <= 4 && (raw & (int64_t{1} << (sz * 8 - 1))))
            raw -= int64_t{1} << (sz * 8);
        args[name] = sz >= 8 ? static_cast<int>(raw) : i32(raw);
        off += static_cast<std::size_t>(sz);
    }
    return args;
}

std::vector<std::vector<uint8_t>> interesting_seeds(const FunctionInfo& fn) {
    int n = param_nbytes(fn.params);
    if (n <= 0) return {{0}};
    std::vector<int64_t> values{0, -1, INT_MAX_32, INT_MIN_32, 1, 4, 31, 32, 100, INT_MAX_32 - 100, INT_MAX_32 - 99};
    std::vector<std::vector<uint8_t>> out;
    std::set<std::vector<uint8_t>> seen;
    auto add = [&](std::vector<uint8_t> b) {
        if (static_cast<int>(b.size()) > n) b.resize(static_cast<std::size_t>(n));
        while (static_cast<int>(b.size()) < n) b.push_back(0);
        if (seen.insert(b).second) out.push_back(std::move(b));
    };
    auto pack_i = [](int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        return std::vector<uint8_t>{static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8),
                                    static_cast<uint8_t>(u >> 16), static_cast<uint8_t>(u >> 24)};
    };
    add(std::vector<uint8_t>(static_cast<std::size_t>(n), 0));
    add(std::vector<uint8_t>(static_cast<std::size_t>(n), 0xFF));
    std::vector<std::string> slots;
    for (auto& [t, name] : fn.params)
        if (!name.empty()) slots.push_back(name);
    if (slots.empty()) slots.emplace_back("_");
    for (auto v : values) {
        std::vector<uint8_t> blob;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto p = pack_i(i32(v));
            blob.insert(blob.end(), p.begin(), p.end());
        }
        add(std::move(blob));
    }
    if (slots.size() >= 2) {
        auto imax = pack_i(static_cast<int32_t>(INT_MAX_32));
        auto z = pack_i(0);
        std::vector<uint8_t> a = imax;
        for (std::size_t i = 1; i < slots.size(); ++i) a.insert(a.end(), z.begin(), z.end());
        add(std::move(a));
        std::vector<uint8_t> b = z;
        b.insert(b.end(), imax.begin(), imax.end());
        for (std::size_t i = 2; i < slots.size(); ++i) b.insert(b.end(), z.begin(), z.end());
        add(std::move(b));
        auto one = pack_i(1);
        std::vector<uint8_t> c = one;
        for (std::size_t i = 1; i < slots.size(); ++i) c.insert(c.end(), z.begin(), z.end());
        add(std::move(c));
    }
    return out;
}

Finding bmc_one(const FunctionInfo& fn, int unwind) {
    auto recs = run_bmc({fn}, unwind);
    if (recs.empty()) {
        auto f = make_find("bmc", laws::ERROR, fn, "", "BMC produced no result", laws::STRENGTH_PROVES);
        return f;
    }
    auto r = recs[0];
    if (!r.function) r.function = fn.name;
    if (r.file.empty()) r.file = fn.file;
    if (!r.line) r.line = fn.line;
    return r;
}

}  // namespace stages_detail

namespace {
void parse_acsl_body(std::string body, Spec& spec) {
    auto pos = body.find("\\result");
    while (pos != std::string::npos) {
        body.replace(pos, 7, "result");
        pos = body.find("\\result", pos + 6);
    }
    std::istringstream ss(body);
    std::string part;
    static Regex kw("^(requires|ensures|invariant|decreases|assigns)\\s+(.+)$");
    while (std::getline(ss, part, ';')) {
        part = strip(part);
        if (part.empty()) continue;
        auto m = match_at(kw, part);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        if (kind == "assigns") continue;
        auto val = strip(m->group(2));
        if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
}

std::vector<std::string> extract_acsl_blocks(const std::string& text) {
    std::vector<std::string> bodies;
    std::size_t i = 0;
    while (i < text.size()) {
        auto start = text.find("/*@", i);
        if (start == std::string::npos) break;
        auto end = text.find("*/", start + 3);
        if (end == std::string::npos) break;
        bodies.push_back(text.substr(start + 3, end - (start + 3)));
        i = end + 2;
    }
    return bodies;
}

std::string acsl_preamble(const std::vector<std::string>& lines, int body_start) {
    int i = body_start - 1;
    bool in_block = false;
    while (i >= 0) {
        auto raw = lines[static_cast<std::size_t>(i)];
        auto s = strip(raw);
        if (in_block) {
            if (raw.find("/*@") != std::string::npos || s.starts_with("/*")) in_block = false;
            --i;
            continue;
        }
        if (s.empty()) {
            --i;
            continue;
        }
        if (s.starts_with("//")) {
            --i;
            continue;
        }
        if (s.ends_with("*/") || s.starts_with("/*") || s.starts_with("*")) {
            if (raw.find("/*") != std::string::npos && raw.find("*/") != std::string::npos) {
                --i;
                continue;
            }
            in_block = true;
            --i;
            continue;
        }
        break;
    }
    std::string out;
    for (int j = i + 1; j < body_start && j < static_cast<int>(lines.size()); ++j) {
        if (!out.empty()) out += '\n';
        out += lines[static_cast<std::size_t>(j)];
    }
    return out;
}

}  // namespace

namespace stages_detail {
Spec parse_comments(const FunctionInfo& fn) {
    Spec spec;
    auto text = read_fn_source(fn);
    if (text.empty()) return spec;
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    int start = std::max(0, (fn.line ? fn.line : 1) - 2);
    int end = (fn.span.second ? fn.span.second : (fn.line ? fn.line : 1) + 40);
    end = std::min(static_cast<int>(lines.size()), end);
    int fn_line = fn.line ? fn.line : 1;
    int body_start = std::max(0, fn_line - 1);
    for (auto& b : extract_acsl_blocks(acsl_preamble(lines, body_start))) parse_acsl_body(b, spec);
    std::string region_body;
    for (int i = body_start; i < end; ++i) {
        if (!region_body.empty()) region_body += '\n';
        region_body += lines[static_cast<std::size_t>(i)];
    }
    for (auto& b : extract_acsl_blocks(region_body)) parse_acsl_body(b, spec);
    static Regex clause("(?://|/\\*|\\*)\\s*(requires|ensures|invariant|decreases|diff)\\s*:\\s*(.+?)(?:\\*/)?\\s*$");
    for (int i = start; i < end; ++i) {
        auto ln = lines[static_cast<std::size_t>(i)];
        auto stripped = strip(ln);
        if (stripped.starts_with("//@")) {
            parse_acsl_body(stripped.substr(3), spec);
            continue;
        }
        auto m = clause.search_match(ln);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        auto val = strip(m->group(2));
        if (val.ends_with("*/")) val = strip(val.substr(0, val.size() - 2));
        if (kind == "diff") {
            auto sp = val.find(' ');
            spec.diff = sp == std::string::npos ? val : val.substr(0, sp);
            if (spec.diff->empty()) spec.diff.reset();
        } else if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
    return spec;
}

}  // namespace stages_detail

}  // namespace prism
