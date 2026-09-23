// Stage thread: file globals written in files that start threads without a mutex.
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
std::set<std::string> file_globals(const std::string& text) {
    int depth = 0;
    std::set<std::string> g;
    static Regex decl(
        "^(?:\\s*(?:static|extern|const|volatile|unsigned|signed|short|long)\\s+)*"
        "(?:(?:struct|enum|union)\\s+\\w+\\s+)?"
        "(?:\\w+\\s+)+(?P<name>[A-Za-z_]\\w*)\\s*(?:=\\s*[^;]+)?;");
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto stripped = strip(line);
        if (stripped.empty() || stripped.starts_with("#")) {
            depth += static_cast<int>(std::count(stripped.begin(), stripped.end(), '{') -
                                      std::count(stripped.begin(), stripped.end(), '}'));
            continue;
        }
        if (depth == 0) {
            if (auto m = match_at(decl, line)) g.insert(m->named("name"));
        }
        depth += static_cast<int>(std::count(stripped.begin(), stripped.end(), '{') -
                                  std::count(stripped.begin(), stripped.end(), '}'));
    }
    return g;
}

bool writes_global(const std::string& body, const std::string& name) {
    return re_search("\\b" + name + "\\s*(?:[\\[\\(]|(?:[+\\-*/%&|^]?=))", body);
}

}  // namespace

std::vector<Finding> run_thread(const std::vector<FunctionInfo>& functions) {
    if (functions.empty()) return {};
    std::map<std::string, std::vector<FunctionInfo>> by_file;
    for (auto& fn : functions) by_file[fn.file].push_back(fn);
    std::vector<Finding> out;
    static Regex thread_api(
        "\\b(?:pthread_create|std::jthread|std::thread|CreateThread|thrd_create)\\b");
    static Regex mutex_re("\\b(?:mtx_lock|mtx_timedlock|pthread_mutex)\\b");
    for (auto& [rel, fns] : by_file) {
        std::string file_text;
        auto p = locate_source(fns.front());
        if (p) file_text = strip_comments_keep_lines(read_text_file(*p));
        else {
            for (auto& fn : fns) {
                if (!file_text.empty()) file_text += '\n';
                file_text += fn.body;
            }
        }
        bool has_api = thread_api.search(file_text);
        if (!has_api)
            for (auto& fn : fns)
                if (thread_api.search(fn.body)) has_api = true;
        if (!has_api) continue;
        auto globals = file_globals(file_text);
        if (globals.empty()) continue;
        std::map<std::string, std::vector<FunctionInfo>> writers;
        for (auto& fn : fns)
            for (auto& g : globals)
                if (writes_global(fn.body, g)) writers[g].push_back(fn);
        for (auto& [g, who] : writers) {
            std::vector<FunctionInfo> unsync;
            for (auto& fn : who)
                if (!mutex_re.search(fn.body)) unsync.push_back(fn);
            if (unsync.size() < 2) continue;
            auto& anchor = unsync[0];
            auto f = make_find("thread", laws::FAILED, anchor, "RACE-SHARED",
                               "global '" + g + "' written from " + std::to_string(unsync.size()) +
                                   " functions without mtx_lock/pthread_mutex",
                               laws::STRENGTH_FINDS);
            f.file = rel;
            f.evidence = g;
            f.extra["global"] = g;
            nlohmann::json w = nlohmann::json::array();
            for (auto& fn : unsync) w.push_back(fn.name);
            f.extra["writers"] = w.dump();
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace prism
