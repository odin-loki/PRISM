// Implementation of prism::log (include/prism/log.hpp). Compiled once into
// the PRISM libraries by src/prism/log.cpp; single-file bootstrap tools
// (z3gen, lean_audit, gen_astlint_discard, fuzz_corpus) define
// PRISM_LOG_STANDALONE and get an inline copy in their own namespace.
#pragma once

#include <atomic>
#include <cstdio>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>

namespace prism::log {
#ifdef PRISM_LOG_STANDALONE
inline namespace standalone {
#endif


namespace {

std::string g_program = "prism";
std::mutex g_program_m;

std::string prefix(std::string_view component) {
    std::lock_guard<std::mutex> lk(g_program_m);
    std::string p = g_program;
    if (!component.empty()) {
        if (!p.empty()) p.push_back(' ');
        p.append(component);
    }
    return p;
}

void stderr_sink(Level level, std::string_view component, std::string_view message) {
    std::string line;
    switch (level) {
        case Level::Trace: line = "[" + std::string(component) + "] "; break;
        case Level::Info: line = prefix(component) + ": "; break;
        case Level::Warn: line = prefix(component) + ": warning: "; break;
        case Level::Error: line = prefix(component) + ": error: "; break;
        case Level::Plain: break;
    }
    line.append(message);
    if (line.empty() || line.back() != '\n') line.push_back('\n');
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

std::atomic<Sink> g_default{&stderr_sink};  // the library build sets FORGE's sink (log.cpp)
std::atomic<Sink> g_sink{&stderr_sink};

}  // namespace

PRISM_LOG_DEF void set_program(std::string_view name) {
    std::lock_guard<std::mutex> lk(g_program_m);
    g_program.assign(name);
}

PRISM_LOG_DEF std::string_view program() {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lk(g_program_m);
    copy = g_program;
    return copy;
}

namespace {

class LineBuf : public std::streambuf {
protected:
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::not_eof(ch);
        auto& b = buf();
        b.push_back(static_cast<char>(ch));
        if (ch == '\n') flush_line();
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) overflow(traits_type::to_int_type(s[i]));
        return n;
    }
    int sync() override {
        if (!buf().empty()) flush_line();
        return 0;
    }

private:
    static std::string& buf() {
        thread_local std::string b;
        return b;
    }
    static void flush_line() {
        std::string line;
        line.swap(buf());
        write(Level::Plain, {}, line);
    }
};

}  // namespace

PRISM_LOG_DEF std::ostream& err_stream() {
    static LineBuf lb;
    static std::ostream os(&lb);
    return os;
}

PRISM_LOG_DEF Sink set_sink(Sink s) { return g_sink.exchange(s ? s : g_default.load()); }

PRISM_LOG_DEF void write(Level level, std::string_view component, std::string_view message) {
    g_sink.load()(level, component, message);
}


#ifdef PRISM_LOG_STANDALONE
}  // namespace standalone
#endif
}  // namespace prism::log
