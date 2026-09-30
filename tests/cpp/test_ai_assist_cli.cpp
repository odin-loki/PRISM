// Roadmap 9.3 / 9.4 assistant features end to end through their command-line
// entry points (`prism triage | ask | draft | regress`, called in process),
// with a fake llama-server on a local socket for the model paths:
//
//   * the pipeline writes triage.json and a "Clusters" section without
//     changing a status; `prism triage` leaves report.json byte-identical;
//   * `prism ask` prints the structured query with the answer (grammar without
//     a model); a model's grammar-constrained query is validated and audited;
//   * `prism draft` links every claim; a model's unlinked claim is rejected;
//   * `prism regress` writes tests under <out>/regression_tests only, and
//     --run without --allow-exec runs nothing (Law 9);
//   * triage uses the embedding endpoint when PRISM_EMBED_SERVER is set;
//   * the Qt GUI's assistant panel keeps its contract (Qt is not built here,
//     so its source is checked) and assistant_reply answers it.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/ai_assist.hpp"
#include "prism/config.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Sets environment variables for one scope and restores them after.
struct EnvScope {
    std::map<std::string, std::optional<std::string>> saved;
    void set(const std::string& k, const std::optional<std::string>& v) {
        if (!saved.count(k)) {
            const char* old = std::getenv(k.c_str());
            saved[k] = old ? std::optional<std::string>(old) : std::nullopt;
        }
        if (v) ::setenv(k.c_str(), v->c_str(), 1);
        else ::unsetenv(k.c_str());
    }
    ~EnvScope() {
        for (auto& [k, v] : saved) {
            if (v) ::setenv(k.c_str(), v->c_str(), 1);
            else ::unsetenv(k.c_str());
        }
    }
};

struct CliResult {
    int rc = -1;
    std::string out;
};

// Runs a subcommand entry point with std::cout captured.
CliResult run_cli(int (*fn)(int, char**), std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream cap;
    auto* old = std::cout.rdbuf(cap.rdbuf());
    CliResult r;
    try {
        r.rc = fn(static_cast<int>(argv.size()), argv.data());
    } catch (...) {
        std::cout.rdbuf(old);
        throw;
    }
    std::cout.rdbuf(old);
    r.out = cap.str();
    return r;
}

#ifndef _WIN32
// llama-server test double: /completion answers the ask grammar with a query
// JSON and the draft grammar with one linked and one unlinked claim;
// /embedding returns a 2-d vector; GET is a health check.
class FakeModelServer {
public:
    FakeModelServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(fd_, 16);
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeModelServer() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<std::string> paths() {
        std::lock_guard<std::mutex> g(mu_);
        return paths_;
    }

private:
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> paths_;

    void serve() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            handle(c);
            ::close(c);
        }
    }

    void handle(int c) {
        std::string req;
        char buf[8192];
        std::size_t head_end = std::string::npos;
        std::size_t need = 0;
        while (true) {
            pollfd p{c, POLLIN, 0};
            if (::poll(&p, 1, 5000) <= 0) return;
            const auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            if (head_end == std::string::npos && (head_end = req.find("\r\n\r\n")) != std::string::npos) {
                std::string lower;
                for (char ch : req.substr(0, head_end)) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                auto cl = lower.find("content-length:");
                need = cl == std::string::npos ? 0 : std::stoul(lower.substr(cl + 15));
            }
            if (head_end != std::string::npos && req.size() >= head_end + 4 + need) break;
        }
        if (head_end == std::string::npos) return;
        const auto sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
        const auto method = req.substr(0, sp1);
        const auto path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        const auto body = req.substr(head_end + 4, need);
        {
            std::lock_guard<std::mutex> g(mu_);
            paths_.push_back(path);
        }
        json reply;
        if (method == "GET") {
            reply = {{"status", "ok"}};
        } else {
            json r = json::parse(body.empty() ? "{}" : body, nullptr, false);
            if (r.is_discarded()) r = json::object();
            if (path == "/embedding") {
                const auto text = r.value("content", std::string());
                reply = {{"embedding", text.find("div") != std::string::npos ? json{1.0, 0.0} : json{0.0, 1.0}}};
            } else {
                const auto grammar = r.value("grammar", std::string());
                std::string content = "{}";
                if (grammar.find("statuses") != std::string::npos)
                    content = json{{"stages", {"bmc"}}, {"statuses", {"FAILED"}}, {"cls", {"DIV"}}, {"file_glob", ""},
                                   {"function_glob", ""}, {"text", ""},          {"group_by", ""}, {"count_only", false}}
                                  .dump();
                else if (grammar.find("links") != std::string::npos)
                    content = json::array({{{"section", "Defects"},
                                            {"text", "Dividing by a zero parameter is undefined."},
                                            {"links", {"verdict:bmc#1"}}},
                                           {{"section", "Defects"},
                                            {"text", "Everything else is proved."},
                                            {"links", {"stage:bmc"}}}})
                                  .dump();
                reply = {{"content", content}};
            }
        }
        const auto b = reply.dump();
        const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b;
        std::size_t off = 0;
        while (off < resp.size()) {
            const auto n = ::send(c, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
            if (n <= 0) break;
            off += static_cast<std::size_t>(n);
        }
    }
};
#endif

constexpr const char* kCalc = R"(int add_big(int x) {
    return x + 100;
}

int divide(int a, int b) {
    return a / b;
}

int ok(int x) {
    return x & 1;
}
)";

// One pipeline run over calc.c (inventory, classify, bmc; no model) shared by
// the cases below; the model endpoints point at a closed port.
struct AssistRun {
    fs::path td, src, out;
    std::string report_text;
    EnvScope env;
    AssistRun() {
        td = fs::temp_directory_path() / "prism-assist-cli";
        std::error_code ec;
        fs::remove_all(td, ec);
        src = td / "src";
        out = td / "out";
        fs::create_directories(src);
        std::ofstream(src / "calc.c") << kCalc;
        env.set("OLLAMA_HOST", "http://127.0.0.1:1");
        env.set("PRISM_LLAMA_SERVER", "http://127.0.0.1:1");
        env.set("PRISM_EMBED_SERVER", std::nullopt);
        prism::Config cfg = prism::default_config();
        cfg.root = src;
        cfg.out = out;
        cfg.llm = false;
        cfg.stages = std::vector<std::string>{"inventory", "classify", "bmc"};
        prism::run_pipeline(cfg);
        report_text = slurp(out / "report.json");
    }
};

}  // namespace

TEST_CASE("ai-assist cli: the pipeline's triage orders only; `prism triage` keeps report.json") {
    AssistRun run;
    REQUIRE(!run.report_text.empty());
    const auto tri = json::parse(slurp(run.out / "triage.json"));
    CHECK(tri["kind"] == "prism-triage");
    CHECK(tri["embedder_note"].get<std::string>().find("NOTRUN") != std::string::npos);
    const auto md = slurp(run.out / "report.md");
    REQUIRE(md.find("## Findings") != std::string::npos);
    REQUIRE(md.find("## Clusters") != std::string::npos);
    CHECK(md.find("## Findings") < md.find("## Clusters"));
    const auto rep = json::parse(run.report_text);
    std::map<std::string, std::string> statuses;
    for (const auto& s : rep["stages"]) {
        std::size_t i = 0;
        for (const auto& f : s["findings"])
            statuses[s["name"].get<std::string>() + "#" + std::to_string(i++)] = f["status"].get<std::string>();
    }
    REQUIRE(!tri["clusters"].empty());
    for (const auto& c : tri["clusters"]) CHECK(c["top_status"] == statuses.at(c["representative"].get<std::string>()));
    auto r = run_cli(prism::ai::triage_main, {run.out.string()});
    CHECK(r.rc == 0);
    CHECK(slurp(run.out / "report.json") == run.report_text);
    CHECK(run_cli(prism::ai::triage_main, {(run.td / "nowhere").string()}).rc == 2);
}

TEST_CASE("ai-assist cli: `prism ask` prints the query with the answer") {
    AssistRun run;
    auto r = run_cli(prism::ai::ask_main,
                     {"failed findings in function divide", "--report", run.out.string(), "--no-llm"});
    CHECK(r.rc == 0);
    CAPTURE(r.out);
    CHECK(r.out.find("query (grammar): {") != std::string::npos);
    CHECK(r.out.find("\"function_glob\":\"divide\"") != std::string::npos);
    CHECK(r.out.find("answer: 1 finding") != std::string::npos);
    auto j = json::parse(
        run_cli(prism::ai::ask_main, {"how many failed", "--report", run.out.string(), "--json", "--no-llm"}).out);
    CHECK(j["translator"] == "grammar");
    CHECK(j["query"]["count_only"] == true);
    CHECK(j["count"].get<int>() >= 2);
    auto e = run_cli(prism::ai::ask_main, {"explain", "bmc#0", "--report", run.out.string(), "--no-llm"});
    CHECK(e.out.find("what") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("ai-assist cli: a model's query is validated and audited; its unlinked draft claim is rejected") {
    AssistRun run;
    FakeModelServer srv;
    run.env.set("PRISM_LLAMA_SERVER", srv.url());
    auto a = run_cli(prism::ai::ask_main,
                     {"division problems from model checking", "--report", run.out.string(), "--json"});
    CAPTURE(a.out);
    auto j = json::parse(a.out);
    CHECK(j["translator"].get<std::string>().rfind("llm:", 0) == 0);
    std::vector<std::string> fns;
    for (const auto& m : j["matches"]) fns.push_back(m["function"].get<std::string>());
    CHECK(fns == std::vector<std::string>{"divide"});
    std::vector<json> audit;
    std::istringstream al(slurp(run.out / "ai_audit.jsonl"));
    for (std::string l; std::getline(al, l);)
        if (l.find_first_not_of(" \t\r") != std::string::npos) audit.push_back(json::parse(l));
    std::vector<json> ask;
    for (auto& x : audit)
        if (x["feature"] == "ask") ask.push_back(x);
    REQUIRE(!ask.empty());
    CHECK(ask.back()["checker_result"] == "accepted");
    CHECK(ask.back()["verdict_effect"] == "none");
    // Drafting with the model: the unlinked "proved" claim is rejected.
    auto d = run_cli(prism::ai::draft_main, {"--report", run.out.string(), "--out", (run.out / "d.md").string()});
    CHECK(d.rc == 0);
    auto dj = json::parse(slurp(run.out / "d.json"));
    CHECK(dj["author"].get<std::string>().rfind("llm:", 0) == 0);
    CHECK(dj["rejected"].size() == 1);
    const auto dmd = slurp(run.out / "d.md");
    CHECK(dmd.substr(0, dmd.find("## Rejected")).find("Everything else is proved") == std::string::npos);
    CHECK(slurp(run.out / "report.json") == run.report_text);
    bool completion = false;
    for (const auto& p : srv.paths()) completion = completion || p == "/completion";
    CHECK(completion);
}

TEST_CASE("ai-assist cli: triage uses the embedding endpoint when PRISM_EMBED_SERVER is set") {
    AssistRun run;
    FakeModelServer srv;
    run.env.set("PRISM_EMBED_SERVER", srv.url());
    const auto dir = run.td / "copy";
    fs::create_directories(dir);
    std::ofstream(dir / "report.json", std::ios::binary) << run.report_text;
    auto r = run_cli(prism::ai::triage_main, {dir.string()});
    CHECK(r.rc == 0);
    const auto tri = json::parse(slurp(dir / "triage.json"));
    CHECK(tri["embedder"].get<std::string>().find("llama-server-embedding") != std::string::npos);
    bool embedding = false;
    for (const auto& p : srv.paths()) embedding = embedding || p == "/embedding";
    CHECK(embedding);
}
#endif

TEST_CASE("ai-assist cli: `prism regress` writes under <out> only; --run without --allow-exec runs nothing") {
    AssistRun run;
    auto r = run_cli(prism::ai::regress_main, {"--report", (run.out / "report.json").string()});
    CHECK(r.rc == 0);
    auto m = json::parse(slurp(run.out / "regression_tests" / "manifest.json"));
    std::set<std::string> written;
    for (const auto& t : m["tests"])
        if (t["status"] == "written") written.insert(t["function"].get<std::string>());
    CHECK(written == std::set<std::string>{"add_big", "divide"});
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(run.src)) names.push_back(e.path().filename().string());
    CHECK(names == std::vector<std::string>{"calc.c"});
    run_cli(prism::ai::regress_main, {"--report", (run.out / "report.json").string(), "--run"});
    m = json::parse(slurp(run.out / "regression_tests" / "manifest.json"));
    for (const auto& t : m["tests"])
        if (t.contains("name") && !t["name"].get<std::string>().empty()) CHECK(t["status"] == "NOTRUN");
}

TEST_CASE("ai-assist cli: the assurance template draft links every claim to the report or a theorem") {
    AssistRun run;
    auto r = run_cli(prism::ai::draft_main, {"--report", run.out.string(), "--kind", "assurance", "--proofs",
                                             (repo_root() / "proofs").string(), "--no-llm"});
    CHECK(r.rc == 0);
    auto d = json::parse(slurp(run.out / "draft_assurance.json"));
    CHECK(d["rejected"].empty());
    for (const auto& s : d["sections"])
        for (const auto& c : s["claims"]) CHECK(!c["links"].empty());
    CHECK(d["theorems_indexed"].get<int>() > 20);
    CHECK(slurp(run.out / "draft_assurance.md").find("[theorem:Prism.proved_bounded_never_merge]") != std::string::npos);
}

TEST_CASE("ai-assist gui: the Qt assistant panel keeps its contract; assistant_reply answers it") {
    // Qt is not built in every configuration: the panel's source contract.
    const auto cpp = slurp(repo_root() / "src" / "gui" / "MainWindow.cpp");
    const auto hdr = slurp(repo_root() / "src" / "gui" / "MainWindow.h");
    REQUIRE(!cpp.empty());
    CHECK(hdr.find("void onAsk();") != std::string::npos);
    CHECK(cpp.find("prism::ai::assistant_reply") != std::string::npos);
    const auto on_ask = cpp.find("void MainWindow::onAsk");
    REQUIRE(on_ask != std::string::npos);
    CHECK(cpp.find("cfg.resume = true;", on_ask) != std::string::npos);
    CHECK(cpp.find("NOTRUN assistant") != std::string::npos);
    CHECK(cpp.find("cellDoubleClicked") != std::string::npos);
    // What the panel shows for a question: the structured query and the findings.
    const auto dir = fs::temp_directory_path() / "prism-assist-gui";
    fs::create_directories(dir);
    std::ofstream(dir / "report.json", std::ios::binary) << R"({"root": "x", "stages": [{"name": "bmc", "status": "ok", "findings": [
        {"stage": "bmc", "status": "FAILED", "file": "a.c", "function": "f", "line": 1, "cls": "INT-DIV-ZERO",
         "message": "div0", "strength": "PROVES", "evidence": "", "counterexample": "x=0", "extra": {}}]}]})";
    auto rep = prism::RunReport::load(dir / "report.json");
    REQUIRE(rep);
    const auto reply = prism::ai::assistant_reply(*rep, "failed division", false);
    CAPTURE(reply);
    CHECK(reply.find("query (grammar)") != std::string::npos);
    CHECK(reply.find("bmc#0 FAILED a.c:1") != std::string::npos);
}
