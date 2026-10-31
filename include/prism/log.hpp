#pragma once

// PRISM's one logging interface. Deliberately thin: a level, a component
// name and a message go to one sink. The default sink writes PRISM's stderr
// lines and forwards every record to FORGE's logger (src/prism/log.cpp);
// tests capture with set_sink(). Nothing else in PRISM
// should write diagnostics to stderr directly.
//
//   error / warn  user-facing problems: "prism <component>: error: <msg>"
//   trace         diagnostics, printed only when PRISM_MEM_TRACE (memory) or
//                 the caller's own switch enables them: "[<component>] <msg>"

#include "export.hpp"  // relative: single-file tools build without -I

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace prism::log {
#ifdef PRISM_LOG_STANDALONE
inline namespace standalone {
#  define PRISM_LOG_API inline
#else
#  define PRISM_LOG_API PRISM_API
#endif

// Plain: the message is written verbatim (usage text, progress lines) and
// maps onto forge::log info with no prefix.
enum class Level { Trace, Info, Warn, Error, Plain };

using Sink = void (*)(Level, std::string_view component, std::string_view message);

// Replace the sink (nullptr restores the stderr default). Returns the old one.
PRISM_LOG_API Sink set_sink(Sink s);

// The program name used as the prefix ("prism" by default; the tools set
// "prism-qa", "prism-deps", "z3gen" ...). An empty component prints
// "<program>: error: <msg>", otherwise "<program> <component>: error: <msg>".
PRISM_LOG_API void set_program(std::string_view name);
PRISM_LOG_API std::string_view program();

PRISM_LOG_API void write(Level level, std::string_view component, std::string_view message);

inline void error(std::string_view component, std::string_view message) { write(Level::Error, component, message); }
inline void warn(std::string_view component, std::string_view message) { write(Level::Warn, component, message); }
inline void info(std::string_view component, std::string_view message) { write(Level::Info, component, message); }
inline void plain(std::string_view message) { write(Level::Plain, {}, message); }
inline void trace(std::string_view component, std::string_view message) { write(Level::Trace, component, message); }

// Build a message from stream-able parts: log::error("qa", log::cat("bad ", n)).
template <class... Ts>
std::string cat(Ts&&... parts) {
    std::ostringstream o;
    (o << ... << std::forward<Ts>(parts));
    return o.str();
}

// An ostream whose complete lines go to the sink as Level::Plain. For code
// that takes a diagnostics std::ostream& (prism_ai::main_dispatch,
// lean_audit::run, usage printers): pass log::err_stream() instead of std::cerr.
PRISM_LOG_API std::ostream& err_stream();

#ifdef PRISM_LOG_STANDALONE
}  // namespace standalone
#endif
}  // namespace prism::log

#ifdef PRISM_LOG_STANDALONE
#  define PRISM_LOG_DEF inline
#  include "log_impl.inl"
#endif
