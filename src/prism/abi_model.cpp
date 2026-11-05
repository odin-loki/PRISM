#include "prism/abi_model.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

namespace prism {

AbiModel abi_lp64() { return AbiModel{}; }

AbiModel abi_ilp32() {
    AbiModel a;
    a.long_w = 32;
    a.size_w = 32;
    a.ptr_w = 32;
    return a;
}

AbiModel abi_llp64() {
    AbiModel a;
    a.long_w = 32;
    a.wchar_w = 16;
    return a;
}

namespace {

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

AbiModel abi_from_data_model(std::string_view data_model) {
    const auto u = upper_ascii(data_model);
    if (u == "ILP32") return abi_ilp32();
    if (u == "LLP64") return abi_llp64();
    return abi_lp64();
}

bool is_ilp32(std::string_view data_model) { return upper_ascii(data_model) == "ILP32"; }

std::string_view Target::data_model() const {
    switch (kind) {
        case TargetKind::linux_ilp32: return "ILP32";
        case TargetKind::windows_llp64: return "LLP64";
        case TargetKind::linux_lp64: break;
    }
    return "LP64";
}

Target target_of(TargetKind k) {
    Target t;
    t.kind = k;
    switch (k) {
        case TargetKind::linux_lp64:
            t.triple = "x86_64-unknown-linux-gnu";
            t.abi = abi_lp64();
            break;
        case TargetKind::linux_ilp32:
            t.triple = "i386-unknown-linux-gnu";
            t.abi = abi_ilp32();
            break;
        case TargetKind::windows_llp64:
            t.triple = "x86_64-pc-windows-msvc";
            t.abi = abi_llp64();
            break;
    }
    return t;
}

const char* host_target_triple() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "x86_64-pc-windows-msvc";
#else
    return "x86_64-unknown-linux-gnu";
#endif
}

std::optional<Target> parse_target(std::string_view triple) {
    const std::string t = lower_ascii(triple);
    // arch-vendor-os-env or arch-os-env
    std::vector<std::string> parts;
    for (size_t p = 0;;) {
        auto q = t.find('-', p);
        parts.push_back(t.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
    const std::string& arch = parts[0];
    const std::string& os = parts.size() == 4 ? parts[2] : parts[1];
    const std::string& env = parts.size() == 4 ? parts[3] : parts[2];
    if (parts.size() == 4 && parts[1] != "pc" && parts[1] != "unknown") return std::nullopt;
    const bool x64 = arch == "x86_64" || arch == "amd64";
    const bool x86 = arch == "i386" || arch == "i486" || arch == "i586" || arch == "i686";
    if (os == "linux" && env == "gnu") {
        if (x64) return target_of(TargetKind::linux_lp64);
        if (x86) return target_of(TargetKind::linux_ilp32);
        return std::nullopt;
    }
    // clang writes the MSVC version into the IR triple (x86_64-pc-windows-msvc19.44.35207)
    if (os == "windows" && env.starts_with("msvc")) {
        for (char c : std::string_view(env).substr(4))
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return std::nullopt;
        if (x64) return target_of(TargetKind::windows_llp64);
        return std::nullopt;  // 32-bit and ARM64 Windows are not supported targets
    }
    return std::nullopt;
}

std::vector<std::string> supported_target_triples() {
    return {"x86_64-unknown-linux-gnu", "i386-unknown-linux-gnu", "x86_64-pc-windows-msvc"};
}

std::optional<Target> resolve_target(std::string_view triple, std::string_view data_model) {
    if (is_ilp32(data_model)) return target_of(TargetKind::linux_ilp32);
    if (triple.empty()) return parse_target(host_target_triple());
    return parse_target(triple);
}

namespace {
std::atomic<int> g_active{-1};  // -1: the host target

const Target& target_ref(TargetKind k) {
    static const std::array<Target, 3> all = {target_of(TargetKind::linux_lp64), target_of(TargetKind::linux_ilp32),
                                              target_of(TargetKind::windows_llp64)};
    return all[static_cast<size_t>(k)];
}
}  // namespace

const Target& active_target() {
    const int k = g_active.load();
    if (k < 0) {
        // outside a run: PRISM_TARGET when it names a supported target, else the host
        if (const char* e = std::getenv("PRISM_TARGET"); e && *e)
            if (auto t = parse_target(e)) return target_ref(t->kind);
        static const TargetKind host = parse_target(host_target_triple())->kind;
        return target_ref(host);
    }
    return target_ref(static_cast<TargetKind>(k));
}

ActiveTargetScope::ActiveTargetScope(TargetKind k) : prev_(g_active.exchange(static_cast<int>(k))) {}
ActiveTargetScope::~ActiveTargetScope() { g_active.store(prev_); }

}  // namespace prism
