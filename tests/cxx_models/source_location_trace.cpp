// Trace of std::source_location. Includes the header the pir stage substitutes
// for libstdc++. current() must report a line other than 0.
#include <source_location>

int main() {
    const std::source_location loc = std::source_location::current();
    if (loc.line() == 0) return 1;
    if (loc.file_name() == nullptr) return 2;
    if (loc.function_name() == nullptr) return 3;
    return 0;
}
