// FORGE inference as PRISM's language-model engine (see include/prism/forge_llm.hpp).
#include "prism/forge_llm.hpp"

#include "prism/ai.hpp"

#include <forge/inference/inference.hpp>

#include <bit>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <span>

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

namespace {
std::string render_turns(const Messages& messages) {
    std::string s;
    for (const auto& [role, content] : messages) s += "<|im_start|>" + role + "\n" + content + "<|im_end|>\n";
    return s;
}
constexpr const char* OPEN_THINK = "<|im_start|>assistant\n<think>\n";
constexpr const char* OPEN_NO_THINK = "<|im_start|>assistant\n<think>\n\n</think>\n\n";

// The assistant opener as token ids. <think> and </think> are single vocabulary tokens
// in the Goedel model, but a GGUF-imported FORGE tokenizer does not list them as special,
// so encode() would split them into "<", "think", ">": look their ids up directly.
std::vector<int> encode_opener(const forge::modelfile::bpe_tokenizer& tok, bool think) {
    const int ts = tok.token_id("<think>"), te = tok.token_id("</think>");
    if (ts < 0 || te < 0) return tok.encode(think ? OPEN_THINK : OPEN_NO_THINK);
    auto ids = tok.encode("<|im_start|>assistant\n");
    auto add = [&](const std::vector<int>& v) { ids.insert(ids.end(), v.begin(), v.end()); };
    ids.push_back(ts);
    if (think) {
        add(tok.encode("\n"));
    } else {
        add(tok.encode("\n\n"));
        ids.push_back(te);
        add(tok.encode("\n\n"));
    }
    return ids;
}

// FORGE's GBNF has no bounded repetition. X{m}, X{m,} and X{m,n} are rewritten:
// m copies of X, then n-m nested optionals when n-m <= 16, else X* (also for {m,}).
// The decoded text is re-validated by PRISM (validate_*), which enforces the bounds.
bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}


// Feeds `ids` in chunks of 256 tokens and then power-of-two pieces (128, 64, ..., 1).
// FORGE caches one execution plan, with its device buffers, per input length and never
// evicts it; feeding whole prompts made one plan per distinct prompt length, which filled
// the 24 GB of an RTX 3090 under Goedel-Prover-V2-32B and spilled into shared memory.
// With these sizes at most nine plans ever exist. Returns the last position's logits.
std::vector<float> feed(forge::inference::session& sess, std::span<const int> ids) {
    std::vector<float> logits;
    std::size_t p = 0;
    while (p < ids.size()) {
        const std::size_t left = ids.size() - p;
        const std::size_t step = left >= 256 ? 256 : std::bit_floor(left);
        logits = sess.forward(ids.subspan(p, step));
        p += step;
    }
    return logits;
}
}  // namespace

std::string render_chat(const Messages& messages, bool think) {
    return render_turns(messages) + (think ? OPEN_THINK : OPEN_NO_THINK);
}

std::string lower_grammar(const std::string& g) {
    std::string out;
    std::vector<std::size_t> groups;      // output offsets of open '('
    std::size_t prim = std::string::npos;  // output offset where the last primary starts
    std::size_t i = 0;
    while (i < g.size()) {
        char c = g[i];
        if (c == '#') {  // comment to end of line
            while (i < g.size() && g[i] != '\n') out += g[i++];
            continue;
        }
        if (c == '"' || c == '[') {
            const char close = c == '"' ? '"' : ']';
            prim = out.size();
            out += g[i++];
            while (i < g.size() && g[i] != close) {
                if (g[i] == '\\' && i + 1 < g.size()) out += g[i++];
                out += g[i++];
            }
            if (i < g.size()) out += g[i++];
            continue;
        }
        if (c == '(') {
            groups.push_back(out.size());
            out += g[i++];
            prim = std::string::npos;
            continue;
        }
        if (c == ')') {
            prim = groups.empty() ? std::string::npos : groups.back();
            if (!groups.empty()) groups.pop_back();
            out += g[i++];
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            prim = out.size();
            while (i < g.size() && ident_char(g[i])) out += g[i++];
            continue;
        }
        if (c == '{' && prim != std::string::npos) {
            std::size_t j = i + 1;
            auto num = [&](int& v) {
                std::size_t b = j;
                v = 0;
                while (j < g.size() && std::isdigit(static_cast<unsigned char>(g[j]))) v = v * 10 + (g[j++] - '0');
                return j > b;
            };
            int m = 0, n = -1;
            bool ok = num(m);
            if (ok && j < g.size() && g[j] == ',') {
                ++j;
                if (!num(n)) n = -1;  // {m,} = unbounded
            } else {
                n = m;
            }
            if (ok && j < g.size() && g[j] == '}' && (n < 0 || n >= m)) {
                const std::string x = out.substr(prim);
                out.resize(prim);
                std::string e;
                for (int k = 0; k < m; ++k) e += (e.empty() ? "" : " ") + x;
                if (n < 0 || n - m > 16) {
                    e += (e.empty() ? "" : " ") + x + "*";
                } else if (n > m) {
                    std::string tail;
                    for (int k = 0; k < n - m; ++k) tail = "(" + x + (tail.empty() ? "" : " " + tail) + ")?";
                    e += (e.empty() ? "" : " ") + tail;
                }
                out += "(" + (e.empty() ? std::string("\"\"") : e) + ")";
                prim = std::string::npos;
                i = j + 1;
                continue;
            }
        }
        if (c == ':' || c == '|') prim = std::string::npos;
        out += g[i++];
    }
    return out;
}

std::string device() {
    if (const char* d = std::getenv("PRISM_DEVICE"); d && *d) return d;
    if (const char* d = std::getenv("FORGE_DEVICE"); d && *d) return d;
    return "cpu";
}

namespace {
std::mutex g_mu;
// Opened models live until the process ends and are never destroyed: tearing down a model
// that holds GPU buffers during static destruction (after the device runtime is gone)
// aborted with a double free at exit (measured with Goedel-Prover-V2-32B on cuda).
std::map<std::string, std::shared_ptr<forge::inference::model>>& g_models =
    *new std::map<std::string, std::shared_ptr<forge::inference::model>>;
std::map<std::string, std::string> g_digests;

std::shared_ptr<forge::inference::model> open_model(const fs::path& p, double* load_s = nullptr) {
    std::lock_guard lk(g_mu);
    auto key = p.string();
    if (auto it = g_models.find(key); it != g_models.end()) return it->second;
    const auto t0 = std::chrono::steady_clock::now();
    auto m = forge::inference::model::open(key);
    m->set_device(device());
    if (load_s) *load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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
        r.error = "missing FORGE model (set PRISM_FPK to a .fpk, e.g. goedel-prover-v2-32b.fpk): " + fpk.string();
        return r;
    }
    try {
        auto model = open_model(fpk, &r.load_s);
        const auto* tok = model->tokenizer();
        if (!tok) {
            r.error = fpk.string() + ": .fpk has no tokenizer section";
            return r;
        }
        forge::inference::session_options so;
        so.context = std::min<forge::i64>(model->max_context(), 8192);
        forge::inference::session sess(model, so);

        const bool think = req.chat && req.think;
        std::vector<int> prompt;
        if (req.chat) {
            prompt = tok->encode(render_turns(req.messages));
            auto op = encode_opener(*tok, think);
            prompt.insert(prompt.end(), op.begin(), op.end());
        } else {
            std::string text;
            for (const auto& m : req.messages) text += m.second + "\n\n";
            prompt = tok->encode(text);
        }
        r.prompt_tokens = prompt.size();
        if (forge::i64(prompt.size()) >= so.context) {
            r.error = "prompt longer than the context (" + std::to_string(prompt.size()) + " tokens)";
            return r;
        }
        int im_end = -1, think_end = -1;
        if (auto ids = tok->encode("<|im_end|>"); ids.size() == 1) im_end = ids[0];
        think_end = tok->token_id("</think>");
        const int eos = tok->eos();
        if (think && think_end < 0) {
            r.error = fpk.string() + ": thinking requested but the tokenizer has no </think> token";
            return r;
        }

        forge::inference::sampler_config sc;
        sc.temperature = req.temperature;
        sc.seed = req.seed;
        forge::inference::sampler smp(sc);

        std::unique_ptr<forge::inference::grammar_constraint> gc;
        if (!req.grammar.empty())
            gc = std::make_unique<forge::inference::grammar_constraint>(
                std::make_shared<const forge::inference::grammar>(lower_grammar(req.grammar)), *tok, int(model->vocab()), eos);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(req.timeout_s * 1000));
        using clock = std::chrono::steady_clock;
        auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };
        std::vector<int> hist(prompt.begin(), prompt.end()), out;
        auto t0 = clock::now();
        auto logits = feed(sess, prompt);
        r.prefill_s = secs(t0);
        t0 = clock::now();
        forge::i64 used = forge::i64(prompt.size());
        if (think) {
            // Free reasoning, no grammar, until the model closes </think> or think_max.
            std::vector<int> th;
            bool closed = false;
            const int tbudget = int(std::min<forge::i64>(req.think_max, so.context - used - 1));
            for (int i = 0; i < tbudget; ++i) {
                int t = smp.sample(logits, hist);
                hist.push_back(t);
                int one[1] = {t};
                logits = sess.forward(one);
                ++used;
                if (t == think_end) {
                    closed = true;
                    break;
                }
                if (t == eos || t == im_end) break;  // the turn ended inside the reasoning
                th.push_back(t);
                if (clock::now() > deadline) {
                    r.error = "timeout";
                    break;
                }
            }
            r.thinking = tok->decode(th);
            r.thinking_tokens = th.size();
            if (!r.error.empty()) {
                r.decode_s = secs(t0);
                return r;
            }
            // Close the block ourselves at the cap (or after a stray end of turn).
            std::vector<int> tail;
            if (!closed) {
                tail = tok->encode("\n");
                tail.push_back(think_end);
            }
            auto nn = tok->encode("\n\n");
            tail.insert(tail.end(), nn.begin(), nn.end());
            r.thinking_capped = !closed;
            if (!tail.empty()) {
                logits = feed(sess, tail);
                hist.insert(hist.end(), tail.begin(), tail.end());
                used += forge::i64(tail.size());
            }
        }
        const int budget = int(std::min<forge::i64>(req.max_new, so.context - used));
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
        r.decode_s = secs(t0);
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
