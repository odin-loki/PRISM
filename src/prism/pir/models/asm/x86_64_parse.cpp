// LLVM inline-asm template parsing and the privileged-instruction list (see x86_64_asm.h).
#include "x86_64_asm.h"

#include <algorithm>
#include <cctype>

namespace prism::pir::asmodel {

namespace {
const std::vector<std::string_view> kPrivileged{
    "rdmsr", "wrmsr", "in", "out", "inb", "inw", "inl", "outb", "outw", "outl", "cli", "sti", "hlt", "cpuid",
    "lgdt", "lidt", "lldt", "ltr", "sgdt", "sidt", "sldt", "str", "invlpg", "invpcid", "wbinvd", "invd",
    "swapgs", "iretq", "iret", "sysretq", "sysret", "sysexit", "rdpmc", "xsetbv", "xgetbv", "clts", "lmsw",
    "smsw", "vmcall", "vmmcall", "vmlaunch", "vmresume", "vmxoff", "vmptrld", "rdtscp", "rdpid", "mwait",
    "monitor", "rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase", "stac", "clac", "ud2", "int3", "syscall", "int"};
}  // namespace

const std::vector<std::string_view>& x86_privileged_list() { return kPrivileged; }

bool x86_is_privileged(const X86Ins& ins) {
    if (std::ranges::find(kPrivileged, std::string_view(ins.mn)) != kPrivileged.end()) return true;
    for (auto& o : ins.ops)
        if (o.find("%cr") != std::string::npos || o.find("%db") != std::string::npos ||
            o.find("%dr") != std::string::npos || o.find("%tr") != std::string::npos)
            return true;
    return false;
}

std::optional<std::vector<X86Ins>> parse_x86_template(std::string_view tmpl) {
    std::string t(tmpl);
    for (const char* esc : {"\\0A", "\\0a", "\\09"}) {
        const std::string e(esc);
        const char* rep = e == "\\09" ? " " : ";";
        for (std::size_t p; (p = t.find(e)) != std::string::npos;) t.replace(p, e.size(), rep);
    }
    for (auto& ch : t) {
        if (ch == '\n' || ch == '\r') ch = ';';
        else if (ch == '\t') ch = ' ';
    }
    if (t.find_first_of("(){}|:#\"\\") != std::string::npos) return std::nullopt;
    std::vector<X86Ins> r;
    std::size_t i = 0;
    while (i <= t.size()) {
        std::size_t j = t.find(';', i);
        if (j == std::string::npos) j = t.size();
        std::string piece = t.substr(i, j - i);
        i = j + 1;
        std::size_t a = piece.find_first_not_of(' ');
        if (a == std::string::npos) {
            if (j >= t.size()) break;
            continue;
        }
        std::string rest = piece.substr(a);
        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
        X86Ins ins;
        auto take_word = [&]() {
            std::size_t e = rest.find(' ');
            std::string w = rest.substr(0, e);
            rest = e == std::string::npos ? std::string() : rest.substr(rest.find_first_not_of(' ', e));
            return w;
        };
        std::string w = take_word();
        if (w == "lock") {
            ins.lock = true;
            if (rest.empty()) return std::nullopt;
            w = take_word();
        }
        for (auto& ch : w) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (w.empty() || !std::isalpha(static_cast<unsigned char>(w[0]))) return std::nullopt;
        if (w == "rep" || w == "repz" || w == "repnz" || w == "repe" || w == "repne" || w == "xacquire" ||
            w == "xrelease" || w == "data16" || w == "notrack")
            return std::nullopt;
        ins.mn = w;
        std::string cur;
        for (char ch : rest) {
            if (ch == ',') {
                ins.ops.push_back(cur);
                cur.clear();
            } else if (ch != ' ') {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) ins.ops.push_back(cur);
        for (auto& o : ins.ops)
            if (o.empty()) return std::nullopt;
        r.push_back(std::move(ins));
        if (j >= t.size()) break;
    }
    if (r.empty()) return std::nullopt;
    return r;
}

}  // namespace prism::pir::asmodel

namespace prism::pir::asmodel {

std::optional<std::string> x86_privileged_in(std::string_view tmpl) {
    std::string t(tmpl);
    for (auto& ch : t)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ';';
    std::size_t i = 0;
    while (i <= t.size()) {
        std::size_t j = t.find(';', i);
        if (j == std::string::npos) j = t.size();
        std::string piece = t.substr(i, j - i);
        i = j + 1;
        // words: skip a lock/rep prefix, take the first mnemonic
        std::size_t p = 0;
        std::string mn, rest;
        for (int k = 0; k < 2; ++k) {
            p = piece.find_first_not_of(' ', p);
            if (p == std::string::npos) break;
            std::size_t e = piece.find(' ', p);
            mn = piece.substr(p, e == std::string::npos ? std::string::npos : e - p);
            rest = e == std::string::npos ? std::string() : piece.substr(e);
            p = e == std::string::npos ? piece.size() : e;
            if (mn != "lock" && mn != "rep" && mn != "repz" && mn != "repnz") break;
        }
        for (auto& ch : mn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        X86Ins ins;
        ins.mn = mn;
        ins.ops.push_back(rest);
        if (!mn.empty() && x86_is_privileged(ins)) return mn;
        if (j >= t.size()) break;
    }
    return std::nullopt;
}

}  // namespace prism::pir::asmodel
