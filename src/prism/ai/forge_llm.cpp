// FORGE mind as PRISM's language-model engine (see include/prism/forge_llm.hpp).
#include "prism/forge_llm.hpp"

#include "prism/ai.hpp"

#include <forge/mind/mind.hpp>

#include <chrono>
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
    return f.gcount() == 8 && std::string(magic, 8) == std::string(forge::pack::magic, 8);
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
std::map<std::string, std::shared_ptr<forge::mind::model>> g_models;
std::map<std::string, std::string> g_digests;

std::shared_ptr<forge::mind::model> open_model(const fs::path& p) {
    std::lock_guard lk(g_mu);
    auto key = p.string();
    if (auto it = g_models.find(key); it != g_models.end()) return it->second;
    auto m = forge::mind::model::open(key);
    m->set_device(device());
    g_models.emplace(key, m);
    return m;
}
}  // namespace

std::string model_digest(const fs::path& fpk) {
    std::lock_guard lk(g_mu);
    auto key = fpk.string();
    if (auto it = g_digests.find(key); it != g_digests.end()) return it->second;
    auto d = ai::sha256_file(fpk);
    g_digests.emplace(key, d);
    return d;
}

Reply complete(const fs::path& fpk, const Request& req) {
    Reply r;
    if (!is_fpk(fpk)) {
        r.error = "missing FORGE model (set PRISM_FPK to a .fpk, e.g. qwen3.5-9b.fpk): " + fpk.string();
        return r;
    }
    try {
        auto model = open_model(fpk);
        const auto* tok = model->tokenizer();
        if (!tok) {
            r.error = fpk.string() + ": .fpk has no tokenizer section";
            return r;
        }
        forge::mind::session_options so;
        so.context = std::min<forge::i64>(model->max_context(), 8192);
        forge::mind::session sess(model, so);

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

        forge::mind::sampler_config sc;
        sc.temperature = req.temperature;
        sc.seed = req.seed;
        forge::mind::sampler smp(sc);

        std::unique_ptr<forge::mind::grammar_constraint> gc;
        if (!req.grammar.empty())
            gc = std::make_unique<forge::mind::grammar_constraint>(
                std::make_shared<const forge::mind::grammar>(req.grammar), *tok, int(model->vocab()), eos);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(req.timeout_s * 1000));
        std::vector<int> hist(prompt.begin(), prompt.end()), out;
        auto logits = sess.forward(prompt);
        const int budget = int(std::min<forge::i64>(req.max_new, so.context - forge::i64(prompt.size())));
        for (int i = 0; i < budget; ++i) {
            if (gc) {
                if (gc->finished()) break;
                gc->apply(logits);
            }
            int t = smp.sample(logits, hist);
            if (t == eos || t == im_end) break;
            if (gc && !gc->accept(t)) {
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
            logits = sess.forward(one);
        }
        r.new_tokens = out.size();
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
        forge::mind::session sess(model);
        auto out = sess.infer({{"input_ids", in}}, forge::i64(ids.size()));
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
