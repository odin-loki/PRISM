#include "prism/stages.hpp"
#include "prism/regex.hpp"

#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prism {
namespace {

const std::unordered_set<std::string> kKw = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "_Generic", "break", "continue",
    "goto", "struct", "union", "enum",
};

const char* kScalarTypes =
    "int|unsigned(?:\\s+int)?|long|short|char|"
    "uint32_t|int32_t|size_t|bool|_Bool";

using Index = std::map<std::pair<std::string, std::string>, const FunctionInfo*>;

std::string strip(std::string s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lstrip_copy(std::string text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    return text.substr(i);
}

std::string squeeze_ws(std::string_view typ) {
    std::string out;
    bool sp = false;
    for (char c : typ) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!sp && !out.empty()) out.push_back(' ');
            sp = true;
        } else {
            out.push_back(c);
            sp = false;
        }
    }
    return out;
}

std::string basename_of(const std::string& file) {
    return std::filesystem::path(file).filename().string();
}

bool is_ident(std::string_view name) {
    static Regex re("[A-Za-z_]\\w*");
    auto m = re.search_match(name, 0);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           static_cast<std::size_t>(m->spans[0].second) == name.size();
}

bool has_calls(std::string_view body) {
    static Regex re("\\b([A-Za-z_]\\w*)\\s*\\(");
    for (auto& m : re.finditer(body)) {
        if (!kKw.contains(m.group(1))) return true;
    }
    return false;
}

bool returns_value(const FunctionInfo& callee) {
    std::string rt = strip(callee.return_type.empty() ? "int" : callee.return_type);
    return rt != "void" && !rt.ends_with(" void");
}

bool inlineable_callee(const FunctionInfo& callee) {
    if (!callee.is_static) return false;
    if (callee.kind != "SCALAR" && callee.kind != "VOID") return false;
    if (has_calls(callee.body)) return false;
    return true;
}

const FunctionInfo* lookup(const Index& index, const std::string& file, const std::string& name) {
    auto it = index.find({basename_of(file), name});
    return it == index.end() ? nullptr : it->second;
}

std::vector<std::string> split_args(std::string args) {
    args = strip(std::move(args));
    if (args.empty()) return {};
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        char ch = args[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == ',' && depth == 0) {
            out.push_back(strip(args.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(strip(args.substr(start)));
    return out;
}

std::optional<std::tuple<std::string, std::string, std::size_t>> parse_call(const std::string& text,
                                                                          std::size_t start) {
    static Regex ident("[A-Za-z_]\\w*");
    if (start > text.size()) return std::nullopt;
    auto m = ident.search_match(std::string_view(text).substr(start), 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    std::string name = m->text;
    if (kKw.contains(name) || !is_ident(name)) return std::nullopt;
    std::size_t pos = start + name.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n')) ++pos;
    if (pos >= text.size() || text[pos] != '(') return std::nullopt;
    int depth = 0;
    for (std::size_t i = pos; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') {
            --depth;
            if (depth == 0) {
                return std::make_tuple(name, text.substr(pos + 1, i - (pos + 1)), i + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> match_balanced(const std::string& text, char open_ch,
                                                                 char close_ch) {
    if (text.empty() || text.front() != open_ch) return std::nullopt;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == open_ch) ++depth;
        else if (text[i] == close_ch) {
            --depth;
            if (depth == 0) {
                return std::make_pair(text.substr(1, i - 1), text.substr(i + 1));
            }
        }
    }
    return std::nullopt;
}

std::pair<std::string, std::string> take_stmt(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == '{' && depth == 0) break;
        else if (ch == ';' && depth == 0) {
            return {text.substr(0, i + 1), text.substr(i + 1)};
        }
    }
    return {text, {}};
}

std::string regex_sub(const Regex& re, const std::string& src, const std::string& repl) {
    std::string out;
    std::size_t i = 0;
    for (auto& m : re.finditer(src)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(m.spans[0].first < 0 ? 0 : m.spans[0].first);
        auto b = static_cast<std::size_t>(m.spans[0].second < 0 ? 0 : m.spans[0].second);
        if (a < i) continue;
        out.append(src, i, a - i);
        for (std::size_t k = 0; k < repl.size(); ++k) {
            if ((repl[k] == '$' || repl[k] == '\\') && k + 1 < repl.size() &&
                std::isdigit(static_cast<unsigned char>(repl[k + 1]))) {
                out += m.group(static_cast<std::size_t>(repl[k + 1] - '0'));
                ++k;
            } else {
                out += repl[k];
            }
        }
        i = b;
    }
    out.append(src, i, std::string::npos);
    return out;
}

std::string rename_params(const std::string& body, const std::map<std::string, std::string>& rename) {
    static Regex ident("[A-Za-z_]\\w*");
    std::string out;
    std::size_t i = 0;
    for (auto& m : ident.finditer(body)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(m.spans[0].first < 0 ? 0 : m.spans[0].first);
        auto b = static_cast<std::size_t>(m.spans[0].second < 0 ? 0 : m.spans[0].second);
        if (a < i) continue;
        out.append(body, i, a - i);
        auto it = rename.find(m.text);
        out += (it != rename.end()) ? it->second : m.text;
        i = b;
    }
    out.append(body, i, std::string::npos);
    return out;
}

std::string adapt_callee_body(std::string body, const std::map<std::string, std::string>& rename,
                              const std::optional<std::string>& ret_var) {
    body = rename_params(body, rename);
    static Regex ret_val("\\breturn\\s+([^;]+);");
    static Regex ret_bare("\\breturn\\s*;");
    if (ret_var) {
        body = regex_sub(ret_val, body, *ret_var + " = $1;");
        body = regex_sub(ret_bare, body, "");
    } else {
        body = regex_sub(ret_bare, body, "");
    }
    return strip(std::move(body));
}

std::string param_type(const std::string& typ) {
    auto t = squeeze_ws(typ.empty() ? "int" : typ);
    return t.empty() ? "int" : t;
}

std::optional<std::string> build_inline_block(const FunctionInfo& callee,
                                              const std::vector<std::string>& args,
                                              const std::string& prefix,
                                              const std::string& tail) {
    if (args.size() != callee.params.size()) return std::nullopt;
    std::vector<std::string> decls;
    std::map<std::string, std::string> rename;
    for (std::size_t i = 0; i < callee.params.size(); ++i) {
        const auto& [typ, pname] = callee.params[i];
        std::string temp = prefix + "_i" + std::to_string(i);
        rename[pname] = temp;
        decls.push_back(param_type(typ) + " " + temp + " = " + args[i] + ";");
    }
    std::optional<std::string> ret_var;
    if (returns_value(callee)) {
        ret_var = prefix + "_ret";
        decls.push_back("int " + *ret_var + ";");
    }
    auto adapted = adapt_callee_body(callee.body, rename, ret_var);
    std::vector<std::string> parts = std::move(decls);
    if (!adapted.empty()) parts.push_back(std::move(adapted));
    if (!tail.empty()) {
        std::string t = tail;
        while (!t.empty() && t.back() == ';') t.pop_back();
        parts.push_back(t + ";");
    }
    std::string inner;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) inner += "\n";
        inner += parts[i];
    }
    return "{\n" + inner + "\n}";
}

std::string try_inline_stmt(const std::string& stmt, const FunctionInfo& fn, const Index& index,
                            int site) {
    const std::string& raw = stmt;
    std::string s = strip(stmt);
    if (s.empty() || s.back() != ';') return raw;

    std::string prefix = "_h" + std::to_string(site);

    static Regex ret_re("return\\s+");
    if (auto m = ret_re.search_match(s, 0); m && !m->spans.empty() && m->spans[0].first == 0) {
        auto end = static_cast<std::size_t>(m->spans[0].second);
        auto call = parse_call(s, end);
        if (!call) return raw;
        auto [name, args_src, call_end] = *call;
        auto rest_src = s.substr(end);
        std::size_t k = 0;
        while (k < rest_src.size() && std::isspace(static_cast<unsigned char>(rest_src[k]))) ++k;
        if (!rest_src.substr(k).starts_with(name)) return raw;
        auto* callee = lookup(index, fn.file, name);
        if (!callee || callee->name == fn.name || !inlineable_callee(*callee)) return raw;
        if (strip(s.substr(call_end)) != ";") return raw;
        auto args = split_args(args_src);
        std::string tail = "return " + std::string(returns_value(*callee) ? prefix + "_ret" : "0");
        auto block = build_inline_block(*callee, args, prefix, tail);
        return block ? *block : raw;
    }

    static Regex assign_re("([A-Za-z_]\\w*)\\s*=\\s*");
    if (auto m = assign_re.search_match(s, 0); m && !m->spans.empty() && m->spans[0].first == 0) {
        std::string lhs = m->group(1);
        auto end = static_cast<std::size_t>(m->spans[0].second);
        auto call = parse_call(s, end);
        if (!call) return raw;
        auto [name, args_src, call_end] = *call;
        auto* callee = lookup(index, fn.file, name);
        if (!callee || callee->name == fn.name || !inlineable_callee(*callee)) return raw;
        if (!returns_value(*callee)) return raw;
        if (strip(s.substr(call_end)) != ";") return raw;
        auto args = split_args(args_src);
        auto block = build_inline_block(*callee, args, prefix, lhs + " = " + prefix + "_ret");
        return block ? *block : raw;
    }

    static Regex decl_re(std::string("(?P<typ>") + kScalarTypes +
                         ")\\s+(?P<name>[A-Za-z_]\\w*)\\s*=\\s*");
    if (auto m = decl_re.search_match(s, 0); m && !m->spans.empty() && m->spans[0].first == 0) {
        std::string lhs = m->named("name");
        auto end = static_cast<std::size_t>(m->spans[0].second);
        auto call = parse_call(s, end);
        if (!call) return raw;
        auto [name, args_src, call_end] = *call;
        auto* callee = lookup(index, fn.file, name);
        if (!callee || callee->name == fn.name || !inlineable_callee(*callee)) return raw;
        if (!returns_value(*callee)) return raw;
        if (strip(s.substr(call_end)) != ";") return raw;
        auto args = split_args(args_src);
        auto block = build_inline_block(*callee, args, prefix, lhs + " = " + prefix + "_ret");
        return block ? *block : raw;
    }

    auto call = parse_call(s, 0);
    if (!call) return raw;
    auto [name, args_src, call_end] = *call;
    if (strip(s.substr(call_end)) != ";") return raw;
    auto* callee = lookup(index, fn.file, name);
    if (!callee || callee->name == fn.name || !inlineable_callee(*callee)) return raw;
    if (returns_value(*callee)) return raw;
    auto args = split_args(args_src);
    auto block = build_inline_block(*callee, args, prefix, "");
    return block ? *block : raw;
}

std::string transform_body(std::string text, const FunctionInfo& fn, const Index& index, int& counter) {
    std::string out;
    while (!text.empty()) {
        text = lstrip_copy(std::move(text));
        if (text.empty()) break;
        if (text.front() == '{') {
            auto matched = match_balanced(text, '{', '}');
            if (!matched) {
                out += text;
                break;
            }
            out += '{';
            out += transform_body(matched->first, fn, index, counter);
            out += '}';
            text = matched->second;
            continue;
        }
        auto [stmt, rest] = take_stmt(text);
        text = rest;
        if (strip(stmt).empty()) continue;
        auto new_stmt = try_inline_stmt(stmt, fn, index, counter);
        if (new_stmt != stmt) ++counter;
        out += new_stmt;
    }
    return out;
}

FunctionInfo inline_function(FunctionInfo fn, const Index& index) {
    if (fn.kind == "POINTER") return fn;
    int counter = 0;
    auto new_body = transform_body(fn.body, fn, index, counter);
    if (counter == 0) return fn;
    fn.body = std::move(new_body);
    return fn;
}

}  // namespace

std::vector<FunctionInfo> inline_static(const std::vector<FunctionInfo>& functions) {
    Index index;
    for (auto& f : functions) index[{basename_of(f.file), f.name}] = &f;
    std::vector<FunctionInfo> out;
    out.reserve(functions.size());
    for (auto& fn : functions) out.push_back(inline_function(fn, index));
    return out;
}

}  // namespace prism
