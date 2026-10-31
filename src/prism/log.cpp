// The default stderr sink of prism::log (include/prism/log.hpp).
#include "prism/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace prism::log {

namespace {

void stderr_sink(Level level, std::string_view component, std::string_view message) {
    std::string line;
    switch (level) {
        case Level::Trace: line = "[" + std::string(component) + "] "; break;
        case Level::Info: line = "prism " + std::string(component) + ": "; break;
        case Level::Warn: line = "prism " + std::string(component) + ": warning: "; break;
        case Level::Error: line = "prism " + std::string(component) + ": error: "; break;
    }
    line.append(message);
    line.push_back('\n');
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

std::atomic<Sink> g_sink{&stderr_sink};

}  // namespace

Sink set_sink(Sink s) { return g_sink.exchange(s ? s : &stderr_sink); }

void write(Level level, std::string_view component, std::string_view message) {
    g_sink.load()(level, component, message);
}

}  // namespace prism::log
