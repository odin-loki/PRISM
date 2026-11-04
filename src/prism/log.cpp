// The library copy of prism::log (include/prism/log.hpp).
#include "prism/log.hpp"

#define PRISM_LOG_DEF
#include "prism/log_impl.inl"

// The default sink is FORGE-backed. PRISM's stderr lines keep
// their fixed, tested format ("prism <component>: error: ..."), and every
// record also goes to forge::log (module "<program>.<component>") so FORGE's
// file, syslog and JSON sinks and FORGE_LOG levels apply. FORGE's own stderr
// sink is switched off: PRISM owns stderr, so nothing prints twice.
#include <forge/core/log.hpp>

namespace prism::log {
namespace {

forge::log::level to_forge(Level l) {
    switch (l) {
        case Level::Trace: return forge::log::level::trace;
        case Level::Warn: return forge::log::level::warn;
        case Level::Error: return forge::log::level::error;
        case Level::Info:
        case Level::Plain: break;
    }
    return forge::log::level::info;
}

void forge_sink(Level level, std::string_view component, std::string_view message) {
    stderr_sink(level, component, message);
    std::string mod(program());
    if (!component.empty()) mod.append(".").append(component);
    if (forge::log::enabled(to_forge(level), mod)) forge::log::emit(to_forge(level), mod, message);
}

const bool g_forge_installed = [] {
    forge::log::set_stderr(false);
    g_default = &forge_sink;
    g_sink = &forge_sink;
    return true;
}();

}  // namespace
}  // namespace prism::log
