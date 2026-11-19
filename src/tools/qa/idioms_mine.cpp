// idioms: the loop idiom miner (docs/TARGETED_PROVING.md W3).
//
//   prism-qa idioms [--acsl DIR] [--json FILE] [--top N] REPORT.json...
//
// Reads the pir rows of PRISM reports (any run with the pir stage). Every
// translated function's row carries extra.loop_shapes: per loop the abstract
// shape (header phis and their updates, exit conditions, memory accesses;
// src/prism/pir/idioms/idioms.cpp) and the library idioms it matches. The
// miner groups the loops by shape and counts, per shape, how the function
// ended: closed by the idiom round (extra.idiom "closed by the idiom
// round"), closed by the generic search (Houdini's generic family,
// k-induction or complete unwinding), violated (FAILED) or open (BOUNDED,
// UNKNOWN, TIMEOUT, ...). The invariants that closed a loop
// (extra.pir_invariants, per loop) are listed with names and constants
// abstracted. A function's outcome is counted for each of its loops.
//
// --acsl DIR: OS-Proving core units (DIR/u*/{meta.tsv,contract.acsl,result.tsv}):
// contract kinds and their most common clause shapes (contracts only: no
// source is read). They are candidates for the contract library (W4), never
// inputs to a proof (Law 4).
//
// Output: a markdown table on stdout; --json FILE the same data as JSON.

#include "qa.hpp"

#include "prism/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace prism::qa {
namespace {

struct ShapeRow {
    std::string shape;
    std::size_t loops = 0;
    std::set<std::string> functions;
    std::map<std::string, std::size_t> idioms;  // matched idiom -> loops
    std::size_t by_idiom = 0, by_generic = 0, violated = 0, open = 0;
    std::map<std::string, std::size_t> invariants;  // abstracted invariant -> loops it closed
    std::vector<std::string> examples;               // file::function (first few)
};

std::string abstract_inv(const std::string& s) {
    static const std::regex var(R"(%[A-Za-z_.$][\w.$]*)"), num(R"(\b-?\d+\b)");
    return std::regex_replace(std::regex_replace(s, var, "v"), num, "c");
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool proved(const std::string& st) { return st.rfind("PROVED", 0) == 0; }

void mine_report(const fs::path& p, std::map<std::string, ShapeRow>& rows, std::size_t& fns, std::size_t& loops) {
    json j;
    try {
        j = json::parse(read_file(p));
    } catch (const std::exception& e) {
        prism::log::error("", "idioms: " + p.string() + ": " + e.what());
        return;
    }
    std::vector<const json*> fs_;
    if (j.contains("stages") && j["stages"].is_array())
        for (auto& st : j["stages"])
            if (st.contains("findings") && st["findings"].is_array())
                for (auto& f : st["findings"]) fs_.push_back(&f);
    if (j.contains("findings") && j["findings"].is_array())
        for (auto& f : j["findings"]) fs_.push_back(&f);
    std::set<std::string> seen;
    for (const json* fp : fs_) {
        const json& f = *fp;
        if (f.value("stage", "") != "pir" || !f.contains("extra") || !f["extra"].is_object()) continue;
        const json& ex = f["extra"];
        if (!ex.contains("loop_shapes") || !ex["loop_shapes"].is_string()) continue;
        const std::string fn =
            f.value("file", std::string("?")) + "::" + (f.contains("function") && f["function"].is_string() ? f["function"].get<std::string>() : "?");
        if (!seen.insert(fn).second) continue;
        json ls;
        try {
            ls = json::parse(ex["loop_shapes"].get<std::string>());
        } catch (...) {
            continue;
        }
        if (!ls.is_array()) continue;
        ++fns;
        const std::string status = f.value("status", "");
        const std::string idiom = ex.contains("idiom") && ex["idiom"].is_string() ? ex["idiom"].get<std::string>() : "";
        json inv = json::array();
        if (ex.contains("pir_invariants") && ex["pir_invariants"].is_string()) {
            try {
                inv = json::parse(ex["pir_invariants"].get<std::string>());
            } catch (...) {
            }
        }
        const bool idiom_closed = proved(status) && idiom.find("closed by the idiom round") != std::string::npos;
        for (std::size_t L = 0; L < ls.size(); ++L) {
            auto& l = ls[L];
            const std::string shape = l.value("shape", std::string("?"));
            auto& r = rows[shape];
            r.shape = shape;
            ++r.loops;
            ++loops;
            r.functions.insert(fn);
            if (r.examples.size() < 3 && std::find(r.examples.begin(), r.examples.end(), fn) == r.examples.end())
                r.examples.push_back(fn);
            if (l.contains("idioms") && l["idioms"].is_array())
                for (auto& n : l["idioms"])
                    if (n.is_string()) ++r.idioms[n.get<std::string>()];
            if (status == "FAILED") ++r.violated;
            else if (idiom_closed) ++r.by_idiom;
            else if (proved(status)) ++r.by_generic;
            else ++r.open;
            if (proved(status) && inv.is_array() && inv.size() == ls.size() && inv[L].is_array()) {
                std::set<std::string> once;
                for (auto& t : inv[L])
                    if (t.is_string() && once.insert(abstract_inv(t.get<std::string>())).second)
                        ++r.invariants[abstract_inv(t.get<std::string>())];
            }
        }
    }
}

struct KindRow {
    std::size_t units = 0, proved = 0, loop_inv = 0;
    std::map<std::string, std::size_t> clauses;
};

std::string abstract_clause(std::string s) {
    static const std::regex id(R"(\b(?!result\b|valid\b|valid_read\b|old\b|true\b|false\b|null\b)[A-Za-z_]\w*(->\w+|\.\w+)*\b)"),
        num(R"(\b(0x[0-9a-fA-F]+|\d+)[uUlL]*\b)"), ws(R"(\s+)");
    s = std::regex_replace(s, id, "x");
    s = std::regex_replace(s, num, "N");
    s = std::regex_replace(s, ws, " ");
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void mine_acsl(const fs::path& dir, std::map<std::string, KindRow>& kinds) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory()) continue;
        auto meta = read_file(e.path() / "meta.tsv");
        if (meta.empty()) continue;
        std::vector<std::string> cols;
        std::stringstream ss(meta.substr(0, meta.find('\n')));
        for (std::string c; std::getline(ss, c, '\t');) cols.push_back(c);
        const std::string kind = cols.size() > 2 ? cols[2] : "?";
        auto& k = kinds[kind];
        ++k.units;
        auto res = read_file(e.path() / "result.tsv");
        {
            std::vector<std::string> rc;
            std::stringstream rs(res.substr(0, res.find('\n')));
            for (std::string c; std::getline(rs, c, '\t');) rc.push_back(c);
            if (rc.size() > 4 && rc[4] == "PROVED") ++k.proved;
        }
        auto acsl = read_file(e.path() / "contract.acsl");
        if (acsl.find("loop invariant") != std::string::npos) ++k.loop_inv;
        // clauses: requires / ensures / assigns ... up to ';'
        static const std::regex clause(R"(\b(requires|ensures|assigns|loop invariant|loop variant)\b([^;]*);)");
        std::set<std::string> once;
        for (auto it = std::sregex_iterator(acsl.begin(), acsl.end(), clause); it != std::sregex_iterator(); ++it) {
            auto a = (*it)[1].str() + " " + abstract_clause((*it)[2].str());
            if (a.size() > 120) a = a.substr(0, 117) + "...";
            if (once.insert(a).second) ++k.clauses[a];
        }
    }
}

std::string pct(std::size_t a, std::size_t b) {
    if (b == 0) return "-";
    std::ostringstream o;
    o << (100 * a + b / 2) / b << "%";
    return o.str();
}

}  // namespace

int idioms_main(const Args& args) {
    std::vector<fs::path> reports;
    fs::path acsl, json_out;
    std::size_t top = 40;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "-h" || a == "--help") {
            std::cout << "usage: prism-qa idioms [--acsl DIR] [--json FILE] [--top N] REPORT.json...\n"
                         "  loop shapes of the pir rows (extra.loop_shapes), ranked by count, with how their\n"
                         "  functions ended: closed by the idiom round, by the generic search, violated, open.\n"
                         "  --acsl DIR  OS-Proving core units: contract kinds and clause shapes (contracts only)\n";
            return 0;
        }
        if (a == "--acsl" && i + 1 < args.size()) acsl = args[++i];
        else if (a == "--json" && i + 1 < args.size()) json_out = args[++i];
        else if (a == "--top" && i + 1 < args.size()) top = static_cast<std::size_t>(std::stoul(args[++i]));
        else reports.push_back(a);
    }
    if (reports.empty() && acsl.empty()) {
        prism::log::error("", "idioms: no report given (prism-qa idioms --help)");
        return 2;
    }
    std::map<std::string, ShapeRow> rows;
    std::size_t fns = 0, loops = 0;
    for (auto& r : reports) mine_report(r, rows, fns, loops);
    std::vector<const ShapeRow*> ranked;
    for (auto& [k, r] : rows) ranked.push_back(&r);
    std::sort(ranked.begin(), ranked.end(), [](const ShapeRow* a, const ShapeRow* b) {
        return a->loops != b->loops ? a->loops > b->loops : a->shape < b->shape;
    });
    std::size_t tot_i = 0, tot_g = 0, tot_v = 0, tot_o = 0, matched = 0;
    for (auto* r : ranked) {
        tot_i += r->by_idiom;
        tot_g += r->by_generic;
        tot_v += r->violated;
        tot_o += r->open;
        if (!r->idioms.empty()) matched += r->loops;
    }
    auto& o = std::cout;
    o << "# Loop idioms\n\n";
    o << reports.size() << " report(s), " << fns << " function(s) with loops, " << loops << " loop(s), "
      << rows.size() << " shape(s); " << matched << " loop(s) (" << pct(matched, loops)
      << ") in shapes some idiom matches.\n\n";
    o << "Loops by outcome of their function: closed by the idiom round " << tot_i << " (" << pct(tot_i, loops)
      << "), by the generic search " << tot_g << " (" << pct(tot_g, loops) << "), violated " << tot_v << ", open "
      << tot_o << " (" << pct(tot_o, loops) << ").\n\n";
    if (!ranked.empty()) {
        o << "| # | shape | loops | fns | idioms matched | idiom | generic | violated | open | invariants that closed it "
             "(top 3) |\n|---|---|---|---|---|---|---|---|---|---|\n";
        std::size_t n = 0;
        for (auto* r : ranked) {
            if (++n > top) break;
            std::string ids;
            for (auto& [k, c] : r->idioms) ids += (ids.empty() ? "" : ", ") + k;
            std::vector<std::pair<std::size_t, std::string>> iv;
            for (auto& [k, c] : r->invariants) iv.push_back({c, k});
            std::sort(iv.begin(), iv.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            std::string ivs;
            for (std::size_t k = 0; k < iv.size() && k < 3; ++k)
                ivs += (k ? "; " : "") + iv[k].second + " (" + std::to_string(iv[k].first) + ")";
            auto esc = [](std::string s) {
                for (std::size_t p = 0; (p = s.find('|', p)) != std::string::npos; p += 2) s.replace(p, 1, "\\|");
                return s;
            };
            o << "| " << n << " | `" << esc(r->shape) << "` | " << r->loops << " | " << r->functions.size() << " | "
              << (ids.empty() ? "-" : ids) << " | " << r->by_idiom << " | " << r->by_generic << " | " << r->violated
              << " | " << r->open << " | " << esc(ivs.empty() ? "-" : ivs) << " |\n";
        }
        o << "\n";
    }
    std::map<std::string, KindRow> kinds;
    if (!acsl.empty()) {
        mine_acsl(acsl, kinds);
        std::size_t units = 0, li = 0;
        for (auto& [k, r] : kinds) {
            units += r.units;
            li += r.loop_inv;
        }
        o << "## ACSL contracts (" << acsl.string() << ")\n\n" << units << " unit(s); " << li
          << " with a loop invariant.\n\n| kind | units | proved | top clause shapes |\n|---|---|---|---|\n";
        std::vector<std::pair<std::string, const KindRow*>> kr;
        for (auto& [k, r] : kinds) kr.push_back({k, &r});
        std::sort(kr.begin(), kr.end(), [](auto& a, auto& b) { return a.second->units > b.second->units; });
        for (auto& [k, r] : kr) {
            std::vector<std::pair<std::size_t, std::string>> cs;
            for (auto& [c, n] : r->clauses) cs.push_back({n, c});
            std::sort(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            std::string t;
            for (std::size_t i = 0; i < cs.size() && i < 4; ++i) {
                std::string c = cs[i].second;
                for (std::size_t p = 0; (p = c.find('|', p)) != std::string::npos; p += 2) c.replace(p, 1, "\\|");
                t += (i ? "; " : "") + std::string("`") + c + "` (" + std::to_string(cs[i].first) + ")";
            }
            o << "| " << k << " | " << r->units << " | " << r->proved << " | " << t << " |\n";
        }
        o << "\n";
    }
    if (!json_out.empty()) {
        json j{{"reports", reports.size()}, {"functions", fns}, {"loops", loops}, {"shapes", json::array()}};
        for (auto* r : ranked) {
            json ids = json::object();
            for (auto& [k, c] : r->idioms) ids[k] = c;
            j["shapes"].push_back({{"shape", r->shape},
                                   {"loops", r->loops},
                                   {"functions", r->functions.size()},
                                   {"idioms", ids},
                                   {"closed_by_idiom", r->by_idiom},
                                   {"closed_by_generic", r->by_generic},
                                   {"violated", r->violated},
                                   {"open", r->open},
                                   {"invariants", r->invariants},
                                   {"examples", r->examples}});
        }
        if (!kinds.empty()) {
            json kj = json::object();
            for (auto& [k, r] : kinds) kj[k] = {{"units", r.units}, {"proved", r.proved}, {"loop_invariants", r.loop_inv}, {"clauses", r.clauses}};
            j["acsl"] = kj;
        }
        std::ofstream(json_out) << j.dump(1) << "\n";
    }
    return 0;
}

}  // namespace prism::qa
