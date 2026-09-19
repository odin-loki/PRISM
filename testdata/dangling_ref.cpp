#include <string>
#include <string_view>

string_view view_bad(void) {
    string s;
    s = "hi";
    return s;
}

string_view view_ok(void) {
    static string s;
    s = "hi";
    return s;
}
