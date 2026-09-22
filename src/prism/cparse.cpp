#include "prism/cparse.hpp"

#include "prism/regex.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace prism {
namespace {

const std::unordered_set<std::string> SCALAR_WORDS = {
    "void", "bool", "_bool", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int_fast8_t", "int_fast16_t", "int_fast32_t", "int_fast64_t",
    "uint_fast8_t", "uint_fast16_t", "uint_fast32_t", "uint_fast64_t",
    "int_least8_t", "int_least16_t", "int_least32_t", "int_least64_t",
    "uint_least8_t", "uint_least16_t", "uint_least32_t", "uint_least64_t",
    "_bool", "wchar_t", "char16_t", "char32_t", "enum",
};

const std::unordered_set<std::string> KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "_Generic",
};

const char* FUNC_HEAD_PAT =
    "(?m)^[ \\t]*"
    "(?P<head>"
    "(?P<mods>(?:(?:static|inline|extern|constexpr|unsigned|signed|"
    "const|volatile|restrict|_Noreturn)\\s+)*)"
    "(?P<ret>(?:(?:struct|enum|union)\\s+)?(?:long\\s+long|[A-Za-z_]\\w*))"
    "(?P<stars>(?:\\s*\\*+\\s*|\\s+))"
    "(?P<name>[A-Za-z_]\\w*)\\s*"
    "\\((?P<params>[^;{}]*?)\\)"
    "(?P<attrs>"
    "(?:"
    "\\s*__attribute__\\s*\\(\\s*\\([^;{}]*?\\)\\s*\\)"
    "|\\s*noexcept(?:\\s*\\([^;{}]*?\\))?"
    "|\\s*throw\\s*\\([^;{}]*?\\)"
    "|\\s*(?:const|volatile|override|final)"
    ")*)"
    "\\s*)"
    "\\{";

bool is_pointer_type(std::string_view typ) {
    return typ.find('*') != std::string_view::npos || typ.find('[') != std::string_view::npos;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::pair<std::string, std::string>> split_params(std::string params) {
    while (!params.empty() && std::isspace(static_cast<unsigned char>(params.front()))) params.erase(params.begin());
    while (!params.empty() && std::isspace(static_cast<unsigned char>(params.back()))) params.pop_back();
    if (params.empty() || params == "void") return {};
    std::vector<std::pair<std::string, std::string>> out;
    std::string raw;
    std::stringstream ss(params);
    while (std::getline(ss, raw, ',')) {
        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
        if (raw.empty() || raw == "...") continue;
        raw = Regex("\\b(const|volatile|restrict|register)\\b").search(raw)
                  ? [&] {
                        std::string r;
                        std::size_t i = 0;
                        Regex re("\\b(const|volatile|restrict|register)\\b");
                        auto tmp = raw;
                        // simple word strip
                        std::string acc;
                        std::string word;
                        for (char c : (raw + " ")) {
                            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') word.push_back(c);
                            else {
                                if (word != "const" && word != "volatile" && word != "restrict" && word != "register") {
                                    if (!acc.empty() && !word.empty()) acc.push_back(' ');
                                    acc += word;
                                }
                                word.clear();
                                if (!std::isspace(static_cast<unsigned char>(c))) acc.push_back(c);
                            }
                        }
                        return acc;
                    }()
                  : raw;
        std::string spaced = raw;
        for (char& c : spaced)
            if (c == '*') { /* keep */ }
        auto m = re_search_match("([A-Za-z_]\\w*)\\s*$", spaced);
        if (!m) {
            out.emplace_back(raw, "");
            continue;
        }
        std::string name = m->group(1);
        std::string typ = spaced.substr(0, static_cast<std::size_t>(std::max(0, m->spans[1].first)));
        while (!typ.empty() && std::isspace(static_cast<unsigned char>(typ.back()))) typ.pop_back();
        if (typ.empty()) typ = raw;
        out.emplace_back(typ, name);
    }
    return out;
}

std::string kind_of(const std::string& ret, const std::string& stars,
                    const std::vector<std::pair<std::string, std::string>>& params) {
    (void)ret;
    (void)stars;
    if (params.empty()) return "VOID";
    auto param_ok = [](std::string t) -> std::string {
        for (char& c : t)
            if (c == '\t') c = ' ';
        if (is_pointer_type(t)) return "POINTER";
        std::vector<std::string> words;
        std::string w;
        for (char c : t + " ") {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') w.push_back(c);
            else if (!w.empty()) {
                words.push_back(w);
                w.clear();
            }
        }
        if (words.empty()) return "OTHER";
        for (auto& word : words) {
            if (!SCALAR_WORDS.contains(word)) {
                if (word == "struct" || word == "union") return "OTHER";
                return "OTHER";
            }
        }
        return "SCALAR";
    };
    bool pointer = false, other = false;
    for (auto& [t, _] : params) {
        auto k = param_ok(t);
        if (k == "POINTER") pointer = true;
        if (k == "OTHER") other = true;
    }
    if (pointer) return "POINTER";
    if (other) return "OTHER";
    return "SCALAR";
}

}  // namespace

bool is_c_ext(std::string_view ext) {
    auto e = to_lower(std::string(ext));
    for (auto* p = C_EXTS; *p; ++p)
        if (e == *p) return true;
    return false;
}

bool is_tu_ext(std::string_view ext) {
    auto e = to_lower(std::string(ext));
    for (auto* p = TU_EXTS; *p; ++p)
        if (e == *p) return true;
    return false;
}

std::string strip_comments_keep_lines(std::string_view text, bool blank_strings) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0, n = text.size();
    bool in_block = false;
    while (i < n) {
        if (in_block) {
            if (i + 1 < n && text[i] == '*' && text[i + 1] == '/') {
                in_block = false;
                i += 2;
            } else {
                out.push_back(text[i] == '\n' ? '\n' : ' ');
                ++i;
            }
            continue;
        }
        if (i + 1 < n && text[i] == '/' && text[i + 1] == '*') {
            in_block = true;
            i += 2;
            continue;
        }
        if (i + 1 < n && text[i] == '/' && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') {
                out.push_back(' ');
                ++i;
            }
            continue;
        }
        char c = text[i];
        if (c == '\'') {
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '\'') {
                if (text[i] == '\\') {
                    out.push_back(text[i]);
                    ++i;
                    if (i < n) {
                        out.push_back(text[i]);
                        ++i;
                    }
                    continue;
                }
                out.push_back(text[i]);
                ++i;
            }
            if (i < n) {
                out.push_back(text[i]);
                ++i;
            }
            continue;
        }
        if (c == '"') {
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '"') {
                if (text[i] == '\\') {
                    if (blank_strings) {
                        out.push_back(' ');
                        ++i;
                        if (i < n) {
                            out.push_back(' ');
                            ++i;
                        }
                    } else {
                        out.push_back(text[i]);
                        ++i;
                        if (i < n) {
                            out.push_back(text[i]);
                            ++i;
                        }
                    }
                    continue;
                }
                out.push_back(blank_strings ? (text[i] == '\n' ? '\n' : ' ') : text[i]);
                ++i;
            }
            if (i < n) {
                out.push_back(text[i]);
                ++i;
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

int match_brace(std::string_view text, int open_idx) {
    int depth = 0;
    int n = static_cast<int>(text.size());
    for (int i = open_idx; i < n; ++i) {
        if (text[static_cast<std::size_t>(i)] == '{') ++depth;
        else if (text[static_cast<std::size_t>(i)] == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return -1;
}

bool body_returns_local_array(std::string_view body) {
    if (body.empty()) return false;
    static Regex local_arr(
        "(?m)^[ \\t]*(?P<static>static\\s+)?"
        "(?:const\\s+|volatile\\s+)*"
        "(?:unsigned\\s+|signed\\s+|long\\s+|short\\s+)*"
        "(?:struct\\s+\\w+|union\\s+\\w+|enum\\s+\\w+|"
        "char|int|short|long|float|double|void|"
        "size_t|ssize_t|ptrdiff_t|uint\\w*|int\\w*|wchar_t|_Bool|bool)\\s+"
        "(?P<name>[A-Za-z_]\\w*)\\s*\\[",
        true);
    static Regex ret_decay("\\breturn\\s+\\(*\\s*([A-Za-z_]\\w*)\\s*\\)*\\s*(?:;|[+\\-])");
    static Regex ret_decay_rhs("\\breturn\\s+[^;]*[+\\-]\\s*\\(*\\s*([A-Za-z_]\\w*)\\s*\\)*\\s*;");
    std::unordered_set<std::string> arrays;
    for (auto& m : local_arr.finditer(body)) {
        if (m.named("static").empty()) arrays.insert(m.named("name"));
    }
    if (arrays.empty()) return false;
    for (auto& m : ret_decay.finditer(body))
        if (arrays.contains(m.group(1))) return true;
    for (auto& m : ret_decay_rhs.finditer(body))
        if (arrays.contains(m.group(1))) return true;
    return false;
}

bool body_needs_pointer_harness(std::string_view body) {
    if (body.empty()) return false;
    static Regex local_ptr(
        "\\b(?:struct\\s+\\w+|union\\s+\\w+|void|char|int|short|long|unsigned|signed"
        "|size_t|uint\\w*|int\\w*|FILE|DIR)\\s+\\*\\s*[A-Za-z_]"
        "|\\b(?:malloc|calloc|realloc|reallocarray|strdup|getenv|fopen"
        "|popen|alloca|__builtin_alloca)\\s*\\(");
    if (local_ptr.search(body)) return true;
    if (re_search("return\\s*\\(*\\s*&", body)) return true;
    return body_returns_local_array(body);
}

std::vector<FunctionInfo> extract_functions(const std::filesystem::path& path, std::string rel) {
    auto text = read_file(path);
    auto stripped = strip_comments_keep_lines(text, true);
    auto bodies = strip_comments_keep_lines(text, false);
    if (rel.empty()) rel = path.string();
    static Regex head(FUNC_HEAD_PAT, true);
    std::vector<FunctionInfo> out;
    for (auto& m : head.finditer(stripped)) {
        auto name = m.named("name");
        auto ret = m.named("ret");
        if (KW.contains(name) || KW.contains(ret)) continue;
        int brace = m.spans.empty() ? -1 : m.spans[0].second - 1;
        // finditer full match ends after '{'; the last char of group 0 is '{'
        if (brace < 0 || static_cast<std::size_t>(brace) >= stripped.size() ||
            stripped[static_cast<std::size_t>(brace)] != '{') {
            auto pos = m.spans[0].second;
            brace = pos - 1;
        }
        int close = match_brace(stripped, brace);
        if (close < 0) continue;
        FunctionInfo fn;
        fn.file = rel;
        fn.name = name;
        fn.body = bodies.substr(static_cast<std::size_t>(brace + 1),
                                static_cast<std::size_t>(close - brace - 1));
        fn.signature = m.named("head");
        // collapse whitespace in signature
        {
            std::string sig;
            bool sp = false;
            for (char c : fn.signature) {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!sp && !sig.empty()) sig.push_back(' ');
                    sp = true;
                } else {
                    sig.push_back(c);
                    sp = false;
                }
            }
            fn.signature = sig;
        }
        fn.line = 1 + static_cast<int>(std::count(stripped.begin(),
                                                  stripped.begin() + std::max(0, m.spans[0].first),
                                                  '\n'));
        fn.params = split_params(m.named("params"));
        fn.return_type = m.named("stars");
        while (!fn.return_type.empty() && std::isspace(static_cast<unsigned char>(fn.return_type.front())))
            fn.return_type.erase(fn.return_type.begin());
        fn.return_type = (fn.return_type + " " + ret);
        while (!fn.return_type.empty() && std::isspace(static_cast<unsigned char>(fn.return_type.front())))
            fn.return_type.erase(fn.return_type.begin());
        while (!fn.return_type.empty() && std::isspace(static_cast<unsigned char>(fn.return_type.back())))
            fn.return_type.pop_back();
        fn.is_static = m.named("mods").find("static") != std::string::npos;
        fn.kind = kind_of(ret, m.named("stars"), fn.params);
        fn.span = {fn.line, 1 + static_cast<int>(std::count(stripped.begin(),
                                                            stripped.begin() + close, '\n'))};
        out.push_back(std::move(fn));
    }
    return out;
}

bool tu_is_empty(const std::filesystem::path& path) {
    auto ext = to_lower(path.extension().string());
    if (!is_tu_ext(ext)) return false;
    return extract_functions(path).empty();
}

std::vector<std::filesystem::path> iter_sources(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_regular_file(root)) return {root};
    const std::unordered_set<std::string> skip{
        ".git", "prism-out", "third_party", "build", "node_modules",
        "__pycache__"};
    if (!std::filesystem::exists(root)) return files;
    for (auto it = std::filesystem::recursive_directory_iterator(root);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        bool skip_dir = false;
        for (auto& part : it->path()) {
            if (skip.contains(part.string())) {
                skip_dir = true;
                break;
            }
        }
        if (skip_dir) continue;
        if (it->is_regular_file() && is_c_ext(it->path().extension().string()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace prism
