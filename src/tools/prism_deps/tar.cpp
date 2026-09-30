// Safe extraction of (git-archive) ustar/pax tar files. The rules follow
// Python tarfile's "data" filter: absolute paths, "..", links that leave the
// destination and device/fifo members are refused (fail closed).
#include "prism/deps.hpp"

#include <fstream>
#include <map>
#include <vector>

namespace prism::deps {

namespace {

struct Member {
    std::string name;
    std::string link;
    char type = '0';
    std::uint64_t size = 0;
    unsigned mode = 0644;
};

std::uint64_t parse_num(const char* p, std::size_t n, const std::string& what) {
    auto u = reinterpret_cast<const unsigned char*>(p);
    if (u[0] & 0x80) {  // GNU base-256
        std::uint64_t v = u[0] & 0x7f;
        for (std::size_t i = 1; i < n; ++i) {
            if (v >> 55) throw FetchError("tar: " + what + " out of range");
            v = (v << 8) | u[i];
        }
        return v;
    }
    std::uint64_t v = 0;
    std::size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) ++i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) {
        if (v >> 60) throw FetchError("tar: " + what + " out of range");
        v = v * 8 + static_cast<std::uint64_t>(p[i] - '0');
    }
    for (; i < n; ++i)
        if (p[i] != ' ' && p[i] != '\0') throw FetchError("tar: bad " + what + " field");
    return v;
}

std::string cstr(const char* p, std::size_t n) {
    std::size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(p, len);
}

// "len key=value\n" records.
std::map<std::string, std::string> parse_pax(const std::string& data) {
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < data.size()) {
        auto sp = data.find(' ', pos);
        if (sp == std::string::npos) throw FetchError("tar: bad pax record");
        std::size_t len = 0;
        for (std::size_t i = pos; i < sp; ++i) {
            if (data[i] < '0' || data[i] > '9') throw FetchError("tar: bad pax record length");
            len = len * 10 + static_cast<std::size_t>(data[i] - '0');
            if (len > data.size()) throw FetchError("tar: bad pax record length");
        }
        if (len == 0 || pos + len > data.size() || data[pos + len - 1] != '\n')
            throw FetchError("tar: bad pax record");
        std::string rec = data.substr(sp + 1, pos + len - 1 - (sp + 1));
        auto eq = rec.find('=');
        if (eq == std::string::npos) throw FetchError("tar: bad pax record");
        out[rec.substr(0, eq)] = rec.substr(eq + 1);
        pos += len;
    }
    return out;
}

// Lexically normalised relative path inside the archive; throws on escape.
fs::path safe_rel(const std::string& name) {
    if (name.empty()) throw FetchError("tar: empty member name");
    if (name[0] == '/' || name[0] == '\\' || (name.size() > 1 && name[1] == ':'))
        throw FetchError("tar: absolute member path " + name);
    fs::path out;
    std::size_t b = 0;
    while (b <= name.size()) {
        auto e = name.find('/', b);
        if (e == std::string::npos) e = name.size();
        std::string part = name.substr(b, e - b);
        b = e + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") throw FetchError("tar: member path leaves the destination: " + name);
        if (part.find('\\') != std::string::npos) throw FetchError("tar: backslash in member path " + name);
        out /= part;
    }
    if (out.empty()) throw FetchError("tar: empty member name");
    return out;
}

// True when a link at rel (relative) pointing to target stays inside dest.
bool link_inside(const fs::path& rel, const std::string& target) {
    if (target.empty() || target[0] == '/') return false;
    int depth = 0;
    std::vector<std::string> parts;
    for (const auto& p : rel.parent_path()) parts.push_back(p.string());
    depth = static_cast<int>(parts.size());
    std::size_t b = 0;
    while (b <= target.size()) {
        auto e = target.find('/', b);
        if (e == std::string::npos) e = target.size();
        std::string part = target.substr(b, e - b);
        b = e + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (--depth < 0) return false;
        } else {
            ++depth;
        }
    }
    return true;
}

// Resolved (symlinks followed) p lies at or below base.
bool resolved_inside(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    auto cp = fs::weakly_canonical(p, ec);
    if (ec) return false;
    auto it = cp.begin();
    for (auto b = base.begin(); b != base.end(); ++b, ++it) {
        if (b->empty()) continue;
        if (it == cp.end() || *it != *b) return false;
    }
    return true;
}

void read_exact(std::ifstream& in, char* p, std::size_t n) {
    in.read(p, static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(in.gcount()) != n) throw FetchError("tar: truncated archive");
}

}  // namespace

fs::path extract_tar(const fs::path& tar, const fs::path& dest) {
    std::ifstream in(tar, std::ios::binary);
    if (!in) throw FetchError("cannot read " + tar.string());
    fs::create_directories(dest);
    const fs::path base = fs::weakly_canonical(dest);
    std::map<std::string, std::string> pax_next;
    std::string long_name, long_link;
    char hdr[512];
    while (true) {
        in.read(hdr, 512);
        auto got = in.gcount();
        if (got == 0) break;  // no end-of-archive blocks: accept, as tarfile does
        if (got != 512) throw FetchError("tar: truncated header");
        bool zero = true;
        for (char c : hdr)
            if (c) {
                zero = false;
                break;
            }
        if (zero) break;
        unsigned sum = 0;
        for (int i = 0; i < 512; ++i)
            sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(hdr[i]);
        if (parse_num(hdr + 148, 8, "checksum") != sum) throw FetchError("tar: header checksum mismatch");
        Member m;
        m.type = hdr[156];
        m.size = parse_num(hdr + 124, 12, "size");
        m.mode = static_cast<unsigned>(parse_num(hdr + 100, 8, "mode") & 07777);
        m.name = cstr(hdr, 100);
        m.link = cstr(hdr + 157, 100);
        if (cstr(hdr + 257, 5) == "ustar") {
            auto prefix = cstr(hdr + 345, 155);
            if (!prefix.empty()) m.name = prefix + "/" + m.name;
        }
        if (m.size > (std::uint64_t(1) << 40)) throw FetchError("tar: member too large");
        auto read_data = [&]() {
            std::string data(static_cast<std::size_t>(m.size), '\0');
            if (m.size) read_exact(in, data.data(), data.size());
            auto pad = (512 - m.size % 512) % 512;
            in.ignore(static_cast<std::streamsize>(pad));
            return data;
        };
        if (m.type == 'g') {  // pax global header (git archive: the commit id)
            read_data();
            continue;
        }
        if (m.type == 'x') {
            pax_next = parse_pax(read_data());
            continue;
        }
        if (m.type == 'L' || m.type == 'K') {
            auto d = read_data();
            (m.type == 'L' ? long_name : long_link) = cstr(d.data(), d.size());
            continue;
        }
        if (!long_name.empty()) m.name = std::exchange(long_name, {});
        if (!long_link.empty()) m.link = std::exchange(long_link, {});
        if (auto it = pax_next.find("path"); it != pax_next.end()) m.name = it->second;
        if (auto it = pax_next.find("linkpath"); it != pax_next.end()) m.link = it->second;
        if (auto it = pax_next.find("size"); it != pax_next.end()) {
            try {
                m.size = std::stoull(it->second);
            } catch (const std::exception&) {
                throw FetchError("tar: bad pax size");
            }
        }
        pax_next.clear();

        const fs::path rel = safe_rel(m.name);
        const fs::path out = dest / rel;
        std::error_code ec;
        // A member may not be written through an earlier link that leaves dest.
        if (!resolved_inside(out.parent_path(), base))
            throw FetchError("tar: member " + m.name + " resolves outside the destination");
        switch (m.type) {
        case '0':
        case '\0':
        case '7': {
            fs::create_directories(out.parent_path(), ec);
            if (ec) throw FetchError("tar: cannot create " + out.parent_path().string());
            if (fs::is_symlink(fs::symlink_status(out, ec))) fs::remove(out, ec);
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            if (!o) throw FetchError("tar: cannot write " + out.string());
            std::uint64_t left = m.size;
            std::vector<char> buf(1 << 16);
            while (left) {
                auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
                read_exact(in, buf.data(), n);
                o.write(buf.data(), static_cast<std::streamsize>(n));
                left -= n;
            }
            in.ignore(static_cast<std::streamsize>((512 - m.size % 512) % 512));
            o.close();
            if (!o) throw FetchError("tar: write failed for " + out.string());
            // data filter: owner rw, no group/other write, no set-id bits.
            auto mode = (m.mode & 0755) | 0600;
            fs::permissions(out, static_cast<fs::perms>(mode), fs::perm_options::replace, ec);
            break;
        }
        case '5':
            read_data();
            fs::create_directories(out, ec);
            if (ec) throw FetchError("tar: cannot create " + out.string());
            break;
        case '2': {
            read_data();
            if (!link_inside(rel, m.link) || !resolved_inside(out.parent_path() / m.link, base))
                throw FetchError("tar: symlink " + m.name + " -> " + m.link + " leaves the destination");
            fs::create_directories(out.parent_path(), ec);
            fs::remove(out, ec);
            fs::create_symlink(m.link, out, ec);
            if (ec) throw FetchError("tar: cannot create symlink " + out.string() + ": " + ec.message());
            break;
        }
        case '1': {
            read_data();
            const fs::path src = dest / safe_rel(m.link);
            fs::create_directories(out.parent_path(), ec);
            fs::copy_file(src, out, fs::copy_options::overwrite_existing, ec);
            if (ec) throw FetchError("tar: bad hard link " + m.name + " -> " + m.link);
            break;
        }
        default:
            throw FetchError(std::string("tar: member ") + m.name + " has unsupported type '" + m.type + "'");
        }
    }
    std::vector<fs::path> tops;
    for (const auto& e : fs::directory_iterator(dest))
        if (e.is_directory()) tops.push_back(e.path());
    if (tops.size() != 1)
        throw FetchError(tar.filename().string() + ": expected one top-level directory, got " +
                         std::to_string(tops.size()));
    return tops[0];
}

}  // namespace prism::deps
