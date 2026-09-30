// CycloneDX 1.5 SBOM from third_party/MANIFEST.toml (roadmap Part 1.3).
//
// Deterministic: no timestamp unless SOURCE_DATE_EPOCH is set (then
// metadata.timestamp is that instant), and the serial number is a UUIDv5 of
// the manifest hash and the version, so the same manifest gives the same
// file. Layout is that of json.dumps(indent=2) with keys in insertion order,
// so the bytes match the SBOMs published before this tool existed.
//
//   linked   -> type "library", scope "required"  (compiled into PRISM)
//   external -> type "application", scope "optional" (separate process, pinned)
//   system   -> type "application", scope "optional" (host tool, not pinned)
#include "prism/deps.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace prism::deps {

namespace {

// Ordered JSON value: enough for the SBOM.
struct J {
    enum Kind { Str, Int, Obj, Arr } kind = Obj;
    std::string s;
    std::int64_t n = 0;
    std::vector<std::pair<std::string, J>> obj;
    std::vector<J> arr;

    static J str(std::string v) {
        J j;
        j.kind = Str;
        j.s = std::move(v);
        return j;
    }
    static J num(std::int64_t v) {
        J j;
        j.kind = Int;
        j.n = v;
        return j;
    }
    static J array(std::vector<J> v = {}) {
        J j;
        j.kind = Arr;
        j.arr = std::move(v);
        return j;
    }
    J& add(std::string k, J v) {
        obj.emplace_back(std::move(k), std::move(v));
        return *this;
    }
    const J* get(std::string_view k) const {
        for (auto& [key, v] : obj)
            if (key == k) return &v;
        return nullptr;
    }
};

J obj() { return J{}; }
J pair(std::string_view a, std::string av, std::string_view b, std::string bv) {
    return obj().add(std::string(a), J::str(std::move(av))).add(std::string(b), J::str(std::move(bv)));
}

// json.dumps default (ensure_ascii=True): non-ASCII as \uXXXX (UTF-16).
void dump_str(const std::string& s, std::string& out) {
    out += '"';
    auto hex4 = [&](unsigned v) {
        char b[8];
        std::snprintf(b, sizeof b, "\\u%04x", v);
        out += b;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        case '\b': out += "\\b"; continue;
        case '\f': out += "\\f"; continue;
        default: break;
        }
        if (c < 0x20) {
            hex4(c);
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            continue;
        }
        std::uint32_t cp = 0;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        cp = extra == 3 ? (c & 0x07) : extra == 2 ? (c & 0x0f) : (c & 0x1f);
        if (extra == 0 || i + static_cast<std::size_t>(extra) >= s.size())
            throw std::runtime_error("sbom: invalid UTF-8 in manifest");
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        i += static_cast<std::size_t>(extra);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            hex4(0xd800 + (cp >> 10));
            hex4(0xdc00 + (cp & 0x3ff));
        } else {
            hex4(cp);
        }
    }
    out += '"';
}

void dump(const J& j, std::string& out, int depth) {
    auto nl = [&](int d) {
        out += '\n';
        out.append(static_cast<std::size_t>(2 * d), ' ');
    };
    switch (j.kind) {
    case J::Str: dump_str(j.s, out); return;
    case J::Int: out += std::to_string(j.n); return;
    case J::Arr:
        if (j.arr.empty()) {
            out += "[]";
            return;
        }
        out += '[';
        for (std::size_t i = 0; i < j.arr.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            dump(j.arr[i], out, depth + 1);
        }
        nl(depth);
        out += ']';
        return;
    case J::Obj:
        if (j.obj.empty()) {
            out += "{}";
            return;
        }
        out += '{';
        for (std::size_t i = 0; i < j.obj.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            dump_str(j.obj[i].first, out);
            out += ": ";
            dump(j.obj[i].second, out, depth + 1);
        }
        nl(depth);
        out += '}';
        return;
    }
}

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::optional<std::string> purl(const Table& c) {
    const std::string url = c.str("url");
    const std::string pre = "https://github.com/";
    if (url.rfind(pre, 0) != 0 || !c.truthy("commit")) return std::nullopt;
    std::string owner_repo = url.substr(pre.size());
    while (!owner_repo.empty() && owner_repo.front() == '/') owner_repo.erase(0, 1);
    while (!owner_repo.empty() && owner_repo.back() == '/') owner_repo.pop_back();
    if (std::count(owner_repo.begin(), owner_repo.end(), '/') != 1) return std::nullopt;
    auto slash = owner_repo.find('/');
    return "pkg:github/" + to_lower(owner_repo.substr(0, slash)) + "/" + to_lower(owner_repo.substr(slash + 1)) +
           "@" + c.str("commit");
}

J licenses(const std::string& spdx) {
    if (spdx.empty() || spdx == "NOASSERTION") return J::array();
    for (const char* op : {" AND ", " OR ", " WITH "})
        if (spdx.find(op) != std::string::npos) return J::array({obj().add("expression", J::str(spdx))});
    return J::array({obj().add("license", obj().add("id", J::str(spdx)))});
}

J component(const Table& c) {
    const std::string kind = c.str("kind");
    J out;
    out.add("type", J::str(kind == "linked" ? "library" : "application"));
    out.add("bom-ref", J::str(kind + ":" + c.str("name")));
    out.add("name", J::str(c.str("name")));
    out.add("version", J::str(c.truthy("version") ? c.str("version") : ""));
    out.add("scope", J::str(kind == "linked" ? "required" : "optional"));
    if (auto lic = licenses(c.truthy("spdx") ? c.str("spdx") : ""); !lic.arr.empty()) out.add("licenses", lic);
    if (auto p = purl(c)) out.add("purl", J::str(*p));
    if (c.truthy("archive_sha256"))
        out.add("hashes", J::array({pair("alg", "SHA-256", "content", c.str("archive_sha256"))}));
    if (c.truthy("url")) out.add("externalReferences", J::array({pair("type", "vcs", "url", c.str("url"))}));
    J props = J::array({pair("name", "prism:kind", "value", kind)});
    auto prop = [&](const char* key, const char* pname) {
        if (c.truthy(key)) props.arr.push_back(pair("name", pname, "value", c.str(key)));
    };
    prop("commit", "prism:commit");
    prop("tag", "prism:tag");
    if (c.truthy("archive_sha256")) props.arr.push_back(pair("name", "prism:archive_format", "value", "git-archive-tar"));
    prop("tree_sha256", "prism:tree_sha256");
    prop("path", "prism:path");
    if (c.truthy("drift")) {
        std::string d;
        for (const auto& line : c.list("drift")) d += (d.empty() ? "" : "; ") + line;
        props.arr.push_back(pair("name", "prism:drift", "value", d));
    }
    prop("licence_note", "prism:licence_note");
    out.add("properties", props);
    return out;
}

// days since 1970-01-01 -> y/m/d (proleptic Gregorian).
void civil(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

}  // namespace

std::string build_sbom(const std::string& manifest_bytes, const std::string& prism_version) {
    const TomlDoc doc = parse_toml(manifest_bytes);
    const std::string msha = sha256_hex(manifest_bytes);
    J comps = J::array();
    for (const auto& c : doc.array("component")) comps.arr.push_back(component(c));

    J meta;
    meta.add("tools", obj().add("components", J::array({obj()
                                                             .add("type", J::str("application"))
                                                             .add("name", J::str("prism-sbom"))
                                                             .add("version", J::str("1"))})));
    meta.add("component", obj()
                              .add("type", J::str("application"))
                              .add("bom-ref", J::str("prism"))
                              .add("name", J::str("prism"))
                              .add("version", J::str(prism_version))
                              .add("licenses", J::array({obj().add("license", obj().add("id", J::str("AGPL-3.0-only")))})));
    meta.add("properties", J::array({pair("name", "prism:manifest_sha256", "value", msha)}));
    if (const char* sde = std::getenv("SOURCE_DATE_EPOCH"); sde && *sde) {
        std::string v(sde);
        if (v.find_first_not_of("0123456789") == std::string::npos && v.size() < 18) {
            const std::int64_t t = std::stoll(v);
            std::int64_t y;
            unsigned mo, d;
            civil(t / 86400, y, mo, d);
            const auto sec = t % 86400;
            char buf[40];
            std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(y), mo, d,
                          static_cast<long long>(sec / 3600), static_cast<long long>(sec / 60 % 60),
                          static_cast<long long>(sec % 60));
            meta.add("timestamp", J::str(buf));
        }
    }
    J linked = J::array();
    for (const auto& c : comps.arr)
        if (c.get("scope")->s == "required") linked.arr.push_back(J::str(c.get("bom-ref")->s));

    J bom;
    bom.add("bomFormat", J::str("CycloneDX"));
    bom.add("specVersion", J::str("1.5"));
    // Fixed namespace for PRISM SBOMs.
    bom.add("serialNumber", J::str("urn:uuid:" + uuid5("5b0e7c52-3a4f-4c33-9d53-7072736d7362", msha + prism_version)));
    bom.add("version", J::num(1));
    bom.add("metadata", meta);
    bom.add("components", comps);
    bom.add("dependencies", J::array({obj().add("ref", J::str("prism")).add("dependsOn", linked)}));
    std::string out;
    dump(bom, out, 0);
    out += '\n';
    return out;
}

}  // namespace prism::deps
