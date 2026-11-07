// FORGE inference as PRISM's language-model engine (see include/prism/forge_llm.hpp).
#include <cstdio>
#include "prism/forge_llm.hpp"

#include "prism/ai.hpp"

#include <forge/inference/inference.hpp>

#include <algorithm>
#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace prism::forge_llm {
namespace fs = std::filesystem;

bool is_fpk(const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec)) return false;
    std::ifstream f(path, std::ios::binary);
    char magic[8] = {};
    f.read(magic, 8);
    return f.gcount() == 8 && std::string(magic, 8) == std::string(forge::modelfile::magic, 8);
}

std::string render_chat(const Messages& messages) {
    std::string s;
    for (const auto& [role, content] : messages) s += "<|im_start|>" + role + "\n" + content + "<|im_end|>\n";
    s += "<|im_start|>assistant\n<think>\n\n</think>\n\n";
    return s;
}

std::string device() {
    if (const char* d = std::getenv("PRISM_DEVICE"); d && *d) return d;
    if (const char* d = std::getenv("FORGE_DEVICE"); d && *d) return d;
    return "cpu";
}

namespace {
std::mutex g_mu;
std::map<std::string, std::shared_ptr<forge::inference::model>> g_models;
std::map<std::string, std::string> g_digests;

// One grammar constraint per (model, grammar) kept for the whole process: it holds the token trie (built once, about 0.2 s) and FORGE's
// per-state mask cache, which makes the grammar mask cheap after the first few tokens (measured 7 Nov 2026: without it the mask took
// 0.3 s per token, 95% of decode time). A concurrent second user of the same entry gets a private constraint.
struct GcEntry {
    std::shared_ptr<const forge::inference::grammar> g;
    std::unique_ptr<forge::inference::grammar_constraint> c;
    bool busy = false;
};
std::map<std::string, GcEntry> g_gc;

struct GcLease {
    forge::inference::grammar_constraint* gc = nullptr;
    std::unique_ptr<forge::inference::grammar_constraint> own;
    GcEntry* entry = nullptr;
    GcLease() = default;
    GcLease(const GcLease&) = delete;
    GcLease& operator=(const GcLease&) = delete;
    ~GcLease() {
        if (entry) {
            std::lock_guard lk(g_mu);
            entry->busy = false;
        }
    }
};

void lease_constraint(GcLease& l, const std::string& key, const std::string& text, const forge::modelfile::bpe_tokenizer& tok,
                      int vocab, int eos) {
    std::lock_guard lk(g_mu);
    auto& e = g_gc[key];
    if (!e.c) {
        e.g = std::make_shared<const forge::inference::grammar>(text);
        e.c = std::make_unique<forge::inference::grammar_constraint>(e.g, tok, vocab, eos);
    }
    if (!e.busy) {
        e.busy = true;
        e.c->reset();
        l.entry = &e;
        l.gc = e.c.get();
    } else {
        l.own = std::make_unique<forge::inference::grammar_constraint>(e.g, tok, vocab, eos);
        l.gc = l.own.get();
    }
}

std::shared_ptr<forge::inference::model> open_model(const fs::path& p) {
    std::lock_guard lk(g_mu);
    auto key = p.string();
    if (auto it = g_models.find(key); it != g_models.end()) return it->second;
    auto m = forge::inference::model::open(key);
    m->set_device(device());
    g_models.emplace(key, m);
    return m;
}
}  // namespace

std::string model_digest(const fs::path& fpk) {
    std::lock_guard lk(g_mu);
    auto key = fpk.string();
    if (auto it = g_digests.find(key); it != g_digests.end()) return it->second;
    // Hashing a 5 GB model took 44 s on every run (measured 7 Nov 2026). The digest is kept in a small file under the cache directory, keyed by
    // the path, size and modification time, so it is recomputed only when the model file changes.
    std::error_code ec;
    const auto size = fs::file_size(fpk, ec);
    const auto mtime = ec ? fs::file_time_type::duration::zero().count() : fs::last_write_time(fpk, ec).time_since_epoch().count();
    fs::path dir;
    if (const char* h = std::getenv("HOME"); h && *h) dir = fs::path(h) / ".cache" / "prism" / "digests";
    fs::path cf;
    const std::string stamp = std::to_string(size) + " " + std::to_string(static_cast<long long>(mtime));
    if (!dir.empty() && !ec) {
        cf = dir / (std::to_string(std::hash<std::string>{}(key)) + ".txt");
        std::ifstream in(cf);
        std::string st1, st2, dg;
        if (in && std::getline(in, st1) && std::getline(in, dg) && st1 == stamp && dg.size() == 64) {
            g_digests.emplace(key, dg);
            return dg;
        }
    }
    auto d = ai::sha256_file(fpk);
    if (!cf.empty() && d.size() == 64) {
        std::error_code ec2;
        fs::create_directories(dir, ec2);
        std::ofstream out(cf, std::ios::trunc);
        out << stamp << "\n" << d << "\n";
    }
    g_digests.emplace(key, d);
    return d;
}

Reply complete(const fs::path& fpk, const Request& req) {
    Reply r;
    if (!is_fpk(fpk)) {
        r.error = "missing FORGE model (set PRISM_FPK to a .fpk, e.g. qwen3.5-9b.fpk): " + fpk.string();
        return r;
    }
    const bool timing = std::getenv("PRISM_TIMING") != nullptr;
    auto T0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) { if (timing) { auto t = std::chrono::steady_clock::now(); std::fprintf(stderr, "[prism-timing] %-12s %8.2f s\n", what, std::chrono::duration<double>(t - T0).count()); T0 = t; } };
    try {
        auto model = open_model(fpk);
        lap("open_model");
        const auto* tok = model->tokenizer();
        if (!tok) {
            r.error = fpk.string() + ": .fpk has no tokenizer section";
            return r;
        }
        forge::inference::session_options so;
        so.context = std::min<forge::i64>(model->max_context(), 8192);
        forge::inference::session sess(model, so);
        lap("session");

        std::string text;
        if (req.chat) text = render_chat(req.messages);
        else
            for (const auto& m : req.messages) text += m.second + "\n\n";
        auto prompt = tok->encode(text);
        r.prompt_tokens = prompt.size();
        if (forge::i64(prompt.size()) >= so.context) {
            r.error = "prompt longer than the context (" + std::to_string(prompt.size()) + " tokens)";
            return r;
        }
        int im_end = -1;
        if (auto ids = tok->encode("<|im_end|>"); ids.size() == 1) im_end = ids[0];
        const int eos = tok->eos();

        forge::inference::sampler_config sc;
        sc.temperature = req.temperature;
        sc.seed = req.seed;
        forge::inference::sampler smp(sc);

        GcLease lease;
        if (!req.grammar.empty()) lease_constraint(lease, fpk.string() + std::string(1, char(0)) + req.grammar, req.grammar, *tok, int(model->vocab()), eos);
        forge::inference::grammar_constraint* gc = lease.gc;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(req.timeout_s * 1000));
        std::vector<int> hist(prompt.begin(), prompt.end()), out;
        lap("grammar+tok");
        auto logits = sess.forward(prompt);
        lap("prefill");
        int max_new = req.max_new;
        if (const char* e = std::getenv("PRISM_MAX_NEW"); e && *e) max_new = std::max(16, std::min(max_new, std::atoi(e)));   // a Lean tactic reply needs far fewer than the default
        const int budget = int(std::min<forge::i64>(max_new, so.context - forge::i64(prompt.size())));
        double t_apply = 0, t_sample = 0, t_fwd = 0, t_accept = 0;
        auto now = [] { return std::chrono::steady_clock::now(); };
        auto el = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
        for (int i = 0; i < budget; ++i) {
            auto a0 = now();
            if (gc) {
                if (gc->finished()) break;
                gc->apply(logits);
            }
            auto a1 = now(); t_apply += el(a0, a1);
            int t = smp.sample(logits, hist);
            t_sample += el(a1, now());
            if (t == eos || t == im_end) break;
            auto c0 = now();
            bool acc_ok = !gc || gc->accept(t);
            t_accept += el(c0, now());
            if (!acc_ok) {
                r.error = "grammar rejected token";
                break;
            }
            out.push_back(t);
            hist.push_back(t);
            if (std::chrono::steady_clock::now() > deadline) {
                r.error = "timeout";
                break;
            }
            int one[1] = {t};
            auto f0 = now();
            logits = sess.forward(one);
            t_fwd += el(f0, now());
        }
        r.new_tokens = out.size();
        lap("decode");
        if (timing) std::fprintf(stderr, "[prism-timing]   apply=%.2f sample=%.2f accept=%.2f forward=%.2f\n", t_apply, t_sample, t_accept, t_fwd);
        if (timing) std::fprintf(stderr, "[prism-timing] prompt_tokens=%zu new_tokens=%zu\n", prompt.size(), out.size());
        r.text = tok->decode(out);
    } catch (const std::exception& ex) {
        r.error = std::string("FORGE: ") + ex.what();
    }
    return r;
}

std::optional<std::vector<double>> embed(const fs::path& fpk, const std::string& text, std::string* err) {
    auto fail = [&](std::string m) -> std::optional<std::vector<double>> {
        if (err) *err = std::move(m);
        return std::nullopt;
    };
    if (!is_fpk(fpk)) return fail("missing embedding model (PRISM_EMBED_FPK): " + fpk.string());
    try {
        auto model = open_model(fpk);
        const auto* tok = model->tokenizer();
        if (!tok) return fail(fpk.string() + ": no tokenizer section");
        auto ids = tok->encode(text);
        if (ids.empty()) return fail("empty input");
        std::vector<float> in(ids.begin(), ids.end());
        forge::inference::session sess(model);
        // A decoder's output is next-token logits, not an embedding.
        if (sess.blueprint().session == "generative_sequence")
            return fail(fpk.string() + ": a generative model, not an embedding model (PRISM_EMBED_FPK wants an encoder .fpk)");
        // FORGE's one_shot infer() has no fixed input name: it binds the names the
        // .fpk blueprint declares (pack::blueprint::inputs, from the ONNX import) and
        // raises on any missing one. Bind token ids to the id input and fill the
        // usual companions (attention mask, token types, positions).
        std::map<std::string, std::vector<float>> feeds;
        const std::size_t n = in.size();
        for (const auto& d : sess.blueprint().inputs) {
            const std::string& nm = d.name;
            if (nm == "attention_mask") feeds[nm].assign(n, 1.0f);
            else if (nm == "token_type_ids") feeds[nm].assign(n, 0.0f);
            else if (nm == "position_ids" || nm == "positions") {
                auto& v = feeds[nm];
                for (std::size_t i = 0; i < n; ++i) v.push_back(float(i));
            } else if (feeds.find("__ids") == feeds.end()) { feeds[nm] = in; feeds["__ids"]; }
            else return fail(fpk.string() + ": embedding graph input '" + nm + "' is not one PRISM can feed");
        }
        feeds.erase("__ids");
        if (feeds.empty()) return fail(fpk.string() + ": embedding graph declares no inputs");
        auto out = sess.infer(feeds, forge::i64(ids.size()));
        if (out.empty() || out.begin()->second.empty()) return fail("embedding graph returned nothing");
        const auto& v = out.begin()->second;
        const std::size_t seq = ids.size(), dim = v.size() % seq == 0 ? v.size() / seq : v.size();
        std::vector<double> e(dim, 0.0);
        const std::size_t rows = v.size() / dim;
        for (std::size_t r = 0; r < rows; ++r)
            for (std::size_t d = 0; d < dim; ++d) e[d] += v[r * dim + d] / double(rows);
        return e;
    } catch (const std::exception& ex) {
        return fail(std::string("FORGE: ") + ex.what());
    }
}

}  // namespace prism::forge_llm
