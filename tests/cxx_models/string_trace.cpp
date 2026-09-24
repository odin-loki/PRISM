// Differential trace of std::string (tests/test_cxx_models.py): compiled once
// against libstdc++ and once with PRISM's model headers
// (src/prism/pir/models/cxx) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS; the two traces must be identical
// (contents, sizes, capacities, whether data() moved, exceptions).
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

static void show(const char* what, const std::string& s) {
    std::printf("%s: size=%zu cap=%zu [%s] nul=%d\n", what, s.size(), s.capacity(), s.c_str(),
                int(s.data()[s.size()] == 0));
}

template <typename F>
static void guarded(const char* what, F f) {
    try {
        f();
        std::printf("%s: ok\n", what);
    } catch (const std::out_of_range& e) {
        std::printf("%s: out_of_range: %s\n", what, e.what());
    } catch (const std::length_error& e) {
        std::printf("%s: length_error: %s\n", what, e.what());
    }
}

int main() {
    std::printf("max_size=%zu npos=%zu\n", std::string().max_size(), std::string::npos);
    std::string a;
    show("default", a);
    std::string b("hello");
    show("cstr", b);
    std::string c("a string longer than the small buffer");
    show("heap", c);
    std::string d(20, 'x');
    show("fill", d);
    std::string e(c, 2, 6);
    show("substr-ctor", e);
    std::string f("abcdef", 3);
    show("ptr-n", f);
    std::string g(b.begin(), b.end());
    show("range", g);
    std::string h{'x', 'y', 'z'};
    show("ilist", h);
    std::string sv(std::string_view("view"));
    show("view", sv);
    std::string cp(c);
    show("copy", cp);
    std::string mv(std::move(cp));
    show("move", mv);
    show("moved-from", cp);
    std::string mvs(std::move(b));
    show("move-sso", mvs);

    // growth policy and data() stability
    std::string s;
    const char* p = s.data();
    for (int i = 0; i < 40; ++i) {
        s.push_back(char('a' + i % 26));
        if (s.data() != p) {
            std::printf("realloc at size=%zu cap=%zu\n", s.size(), s.capacity());
            p = s.data();
        }
    }
    s.reserve(100);
    show("reserve", s);
    s.reserve(10);
    show("reserve-small", s);
    s.shrink_to_fit();
    show("shrink", s);
    s.resize(5);
    show("resize-down", s);
    s.resize(8, '!');
    show("resize-up", s);
    s.shrink_to_fit();
    show("shrink-sso", s);
    s.clear();
    show("clear", s);

    // modifiers
    std::string m("0123456789");
    m.append("abc");
    show("append", m);
    m.append(3, '-');
    show("append-n", m);
    m += 'Z';
    m += "yx";
    m += std::string("W");
    show("+=", m);
    m.insert(2, "__");
    show("insert", m);
    m.insert(m.begin(), '<');
    show("insert-it", m);
    m.insert(m.end(), 2, '>');
    show("insert-n", m);
    m.erase(3, 4);
    show("erase", m);
    m.erase(m.begin() + 1);
    show("erase-it", m);
    m.erase(m.begin(), m.begin() + 2);
    show("erase-range", m);
    m.replace(1, 3, "REPLACED-LONGER");
    show("replace", m);
    m.replace(0, 5, "s");
    show("replace-shorter", m);
    m.replace(m.begin(), m.begin() + 2, 3, '*');
    show("replace-n", m);
    m.assign("assigned");
    show("assign", m);
    m.assign(c, 5, 10);
    show("assign-sub", m);
    m.assign(30, 'q');
    show("assign-n", m);
    m = "x";
    show("op=", m);
    m.pop_back();
    show("pop", m);
    std::string sw1("short"), sw2("a long string for swapping around");
    sw1.swap(sw2);
    show("swap1", sw1);
    show("swap2", sw2);
    std::string self("self-append");
    self.append(self);
    show("self-append", self);
    self.replace(0, 4, self, 5, 6);
    show("self-replace", self);
    self.insert(0, self.c_str() + 3, 4);
    show("self-insert", self);

    // access and search
    std::string t("hello world, hello");
    std::printf("at=%c front=%c back=%c idx=%c\n", t.at(4), t.front(), t.back(), t[6]);
    std::printf("find=%zu %zu %zu %zu\n", t.find("hello"), t.find("hello", 1), t.find('z'), t.find(""));
    std::printf("rfind=%zu %zu %zu\n", t.rfind("hello"), t.rfind('o', 5), t.rfind("xyz"));
    std::printf("ffo=%zu flo=%zu ffno=%zu flno=%zu\n", t.find_first_of("ow"), t.find_last_of("ow"),
                t.find_first_not_of("hel"), t.find_last_not_of("hello"));
    std::printf("compare=%d %d %d %d\n", t.compare("hello") > 0, t.compare(0, 5, "hello"), t.compare("zz") < 0,
                t.compare(6, 5, std::string("world")));
    std::printf("starts=%d ends=%d contains=%d\n", int(t.starts_with("hell")), int(t.ends_with("llo")),
                int(t.contains("world")));
    std::printf("substr=[%s] [%s]\n", t.substr(6, 5).c_str(), t.substr(13).c_str());
    char buf[8] = {};
    std::printf("copy=%zu [%s]\n", t.copy(buf, 5, 6), buf);
    std::printf("eq=%d lt=%d cat=[%s]\n", int(t == "hello world, hello"), int(std::string("a") < std::string("b")),
                (std::string("con") + "cat" + 'e' + std::string("nated")).c_str());
    std::printf("stoi=%d to_string=[%s]\n", std::stoi("42"), std::to_string(-17).c_str());

    // exceptions
    guarded("at", [&] { (void)t.at(t.size()); });
    guarded("substr", [&] { (void)t.substr(t.size() + 1); });
    guarded("substr-end", [&] { (void)t.substr(t.size()); });
    guarded("erase", [&] { t.erase(100); });
    guarded("insert", [&] { t.insert(100, "x"); });
    guarded("replace", [&] { t.replace(100, 1, "x"); });
    guarded("compare", [&] { (void)t.compare(100, 1, "x"); });
    guarded("copy", [&] { (void)t.copy(buf, 1, 100); });
    guarded("ctor", [&] { std::string x(t, 100); });
    guarded("assign", [&] { std::string x; x.assign(t, 100, 1); });
    guarded("append", [&] { std::string x; x.append(t, 100, 1); });
    guarded("reserve", [&] { std::string x; x.reserve(std::string::npos); });
    guarded("resize", [&] { std::string x; x.resize(std::string::npos); });
    guarded("append-n", [&] { std::string x("ab"); x.append(std::string::npos - 1, 'c'); });
    return 0;
}
