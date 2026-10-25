#include <string>

void string_data_bad(void) {
    std::string s;
    const char *p = s.c_str();
    s.clear();
    (void)*p;
}

void string_data_ok(void) {
    std::string s;
    const char *p = s.c_str();
    (void)*p;
    s.clear();
}
