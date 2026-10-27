// Witness YAML scalars (src/prism/svcomp/witness.cpp, to_yaml / scalar).
// A colon, a double quote, a newline, or a leading YAML indicator is
// double-quoted. A newline inside the quotes is `\n`. A byte that is not
// printable ASCII does not split a line: well-formed UTF-8 is kept in the
// quotes, YAML line breaks are `\u` escapes, and a byte that is not
// well-formed UTF-8 is `\xHH`. The witness is still written.
//
// build_correctness_witness only serializes the invariants it is given.
// correctness_invariants already exports them for PROVED, PROVED-UNBOUNDED
// or PROVED-CERTIFIED with k_induction closed-invariants. BOUNDED and
// HYPOTHESIS are not that set. The default data model stays LP64.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/svcomp_witness.hpp"
#include "prism/taskdef.hpp"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace prism::svcomp;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
#ifdef _WIN32
        const long pid = static_cast<long>(_getpid());
#else
        const long pid = static_cast<long>(::getpid());
#endif
        path = fs::temp_directory_path() /
               ("prism_svcomp_witness_" + std::to_string(pid) + "_" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        std::ofstream(path / name, std::ios::binary) << text;
        return path / name;
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ojson obj_value(const std::string& s) {
    ojson doc = ojson::object();
    doc["value"] = s;
    return doc;
}

void expect_roundtrip(const ojson& doc) {
    const std::string y = to_yaml(doc);
    auto back = prism::taskdef::parse_yaml(y);
    REQUIRE_MESSAGE(back.value.has_value(), back.error);
    CHECK(*back.value == doc);
}

// One physical line per YAML line: no raw C0 / DEL, so a break cannot fold
// a quoted scalar. High bytes (UTF-8) are allowed.
void expect_no_raw_break(const std::string& yaml) {
    bool raw = yaml.find('\r') != std::string::npos;
    for (char ch : yaml) {
        const auto c = static_cast<unsigned char>(ch);
        if (c != '\n' && (c < 0x20 || c == 0x7F)) raw = true;
    }
    CHECK_FALSE(raw);
}

WitnessMeta test_meta(const fs::path& src) {
    WitnessMeta m;
    m.input_file = src;
    m.input_file_name = "t.c";
    m.specification = "LTL: G ! overflow\n";
    m.producer_version = "test";
    m.creation_time = "2026-09-23T00:00:00Z";
    m.uuid = "00000000-0000-4000-8000-000000000000";
    return m;
}

std::string bytes_of(std::initializer_list<unsigned char> bs) {
    std::string s;
    s.reserve(bs.size());
    for (unsigned char b : bs) s.push_back(static_cast<char>(b));
    return s;
}

}  // namespace

TEST_CASE("svcomp witness yaml: plain identifiers stay plain; reserved words stay quoted") {
    CHECK(WitnessMeta{}.data_model == "LP64");
    ojson plain = ojson::object();
    plain["function"] = "main";
    plain["file_name"] = "t.c";
    plain["data_model"] = "LP64";
    plain["line"] = 3;
    plain["flag"] = true;
    CHECK(to_yaml(plain) ==
          "function: main\nfile_name: t.c\ndata_model: LP64\nline: 3\nflag: true\n");
    expect_roundtrip(plain);

    CHECK(to_yaml(obj_value("true")) == "value: \"true\"\n");
    CHECK(to_yaml(obj_value("null")) == "value: \"null\"\n");
    CHECK(to_yaml(obj_value("~")) == "value: \"~\"\n");
    CHECK(to_yaml(obj_value(".inf")) == "value: \".inf\"\n");
    expect_roundtrip(obj_value(".inf"));
    expect_roundtrip(obj_value("true"));
}

TEST_CASE("svcomp witness yaml: colon, quote, newline and a leading indicator are quoted") {
    expect_roundtrip(obj_value("a:b"));
    expect_roundtrip(obj_value("a: b"));
    expect_roundtrip(obj_value("say \"hi\""));
    CHECK(to_yaml(obj_value("a:b")) == "value: \"a:b\"\n");
    CHECK(to_yaml(obj_value("a: b")) == "value: \"a: b\"\n");
    CHECK(to_yaml(obj_value("say \"hi\"")) == "value: \"say \\\"hi\\\"\"\n");
    CHECK(to_yaml(obj_value("a b")) == "value: \"a b\"\n");
    CHECK(to_yaml(obj_value("2.0")) == "value: \"2.0\"\n");
    expect_roundtrip(obj_value("2.0"));

    const std::string nl = to_yaml(obj_value("a\nb"));
    CHECK(nl == "value: \"a\\nb\"\n");
    CHECK(nl.find('\n') == nl.size() - 1);
    expect_roundtrip(obj_value("a\nb"));

    const char* leads[] = {"-", "?", ":", ",", "[", "]", "{", "}", "#", "&", "*", "!", "|",
                           ">", "'", "\"", "%", "@", "`"};
    for (const char* lead : leads) {
        const std::string text = std::string(lead) + "x";
        const std::string y = to_yaml(obj_value(text));
        CHECK(y.starts_with("value: \""));
        CHECK(y.find('\n') == y.size() - 1);
        auto back = prism::taskdef::parse_yaml(y);
        REQUIRE_MESSAGE(back.value.has_value(), back.error);
        CHECK((*back.value)["value"] == text);
    }

    ojson key = ojson::object();
    key["a:b"] = 1;
    CHECK(to_yaml(key) == "\"a:b\": 1\n");
    expect_roundtrip(key);
}

TEST_CASE("svcomp witness yaml: non-ASCII stays on one line and a bad byte is kept") {
    std::string text = ":a \"b\"\n\t\r";
    text.push_back('\x01');
    text.push_back('\x1c');
    text.push_back('\\');
    text += bytes_of({0xC3, 0xA9});              // é
    text += bytes_of({0xE2, 0x86, 0x92});        // U+2192
    text += bytes_of({0xF0, 0x9F, 0x98, 0x80});  // U+1F600
    text += bytes_of({0xC2, 0x85});              // U+0085
    text += bytes_of({0xE2, 0x80, 0xA8});        // U+2028
    text += bytes_of({0xE2, 0x80, 0xA9});        // U+2029

    const std::string y = to_yaml(obj_value(text));
    expect_no_raw_break(y);
    CHECK(y.find("\\n") != std::string::npos);
    CHECK(y.find("\\x0a") == std::string::npos);
    CHECK(y.find("\\r") != std::string::npos);
    CHECK(y.find("\\x0d") == std::string::npos);
    CHECK(y.find("\\t") != std::string::npos);
    CHECK(y.find("\\x01") != std::string::npos);
    CHECK(y.find("\\x1c") != std::string::npos);
    CHECK(y.find(bytes_of({0xC3, 0xA9})) != std::string::npos);
    CHECK(y.find(bytes_of({0xE2, 0x86, 0x92})) != std::string::npos);
    CHECK(y.find(bytes_of({0xF0, 0x9F, 0x98, 0x80})) != std::string::npos);
    CHECK(y.find(bytes_of({0xC2, 0x85})) == std::string::npos);
    CHECK(y.find(bytes_of({0xE2, 0x80, 0xA8})) == std::string::npos);
    CHECK(y.find(bytes_of({0xE2, 0x80, 0xA9})) == std::string::npos);
    CHECK(y.find("\\u0085") != std::string::npos);
    CHECK(y.find("\\u2028") != std::string::npos);
    CHECK(y.find("\\u2029") != std::string::npos);
    expect_roundtrip(obj_value(text));

    const std::string overlong = bytes_of({0xC0, 0x80});
    const std::string oy = to_yaml(obj_value(overlong));
    CHECK(oy == "value: \"\\xc0\\x80\"\n");
    CHECK(oy.find(bytes_of({0xC0})) == std::string::npos);
    CHECK(oy.find(bytes_of({0x80})) == std::string::npos);
    expect_no_raw_break(oy);

    std::string bad = "i >= 0";
    bad.push_back(static_cast<char>(0xFF));
    bad += " && i < n";
    const std::string by = to_yaml(obj_value(bad));
    CHECK(by.find(bytes_of({0xFF})) == std::string::npos);
    CHECK(by.find("\\xff") != std::string::npos);
    expect_no_raw_break(by);
    auto back = prism::taskdef::parse_yaml(by);
    REQUIRE_MESSAGE(back.value.has_value(), back.error);
    CHECK((*back.value)["value"] == "i >= 0" + bytes_of({0xC3, 0xBF}) + " && i < n");
}

TEST_CASE("svcomp witness yaml: invariant and counterexample strings stay one document") {
    TempDir d;
    auto src = d.write("t.c", "int main(void) { return 0; }\n");
    auto meta = test_meta(src);
    CHECK(meta.data_model == "LP64");

    std::string inv = ":i >= 0 && s == \"x\"\n";
    inv += bytes_of({0xC3, 0xA9});
    inv.push_back('\r');
    inv += bytes_of({0xE2, 0x80, 0xA8});
    Invariant invariant{"loop_invariant", Location{"*t.c", 3, 1, "main"}, inv};
    auto correct = build_correctness_witness({invariant}, meta);
    const std::string cy = to_yaml(correct);
    expect_no_raw_break(cy);
    CHECK(cy.find("entry_type: invariant_set") != std::string::npos);
    CHECK(cy.find("data_model: LP64") != std::string::npos);
    CHECK(cy.find("value: \"") != std::string::npos);
    CHECK(cy.find("\\n") != std::string::npos);
    CHECK(cy.find("\\r") != std::string::npos);
    CHECK(cy.find(bytes_of({0xE2, 0x80, 0xA8})) == std::string::npos);
    CHECK(cy.find("\\u2028") != std::string::npos);
    CHECK(cy.find("\"*t.c\"") != std::string::npos);
    expect_roundtrip(correct);

    std::string file = "t:c\n\"*";
    Counterexample cex{"main", Location{file, 4, std::nullopt, std::string("*reach")}, {}};
    cex.nondet.push_back(NondetValue{Location{"t.c", 1, 8, std::nullopt}, Num::of_int(-3)});
    auto violation = build_violation_witness(cex, meta);
    const std::string vy = to_yaml(violation);
    expect_no_raw_break(vy);
    CHECK(vy.find("entry_type: violation_sequence") != std::string::npos);
    CHECK(vy.find("data_model: LP64") != std::string::npos);
    CHECK(vy.find("\\\\result == -3") != std::string::npos);
    CHECK(vy.find("\"*reach\"") != std::string::npos);
    const auto marker = std::string("file_name: \"t:c\\n\\\"*\"");
    auto vline_at = vy.find(marker);
    REQUIRE(vline_at != std::string::npos);
    auto vline_end = vy.find('\n', vline_at);
    REQUIRE(vline_end != std::string::npos);
    const auto file_line = vy.substr(vline_at, vline_end - vline_at);
    CHECK(file_line == marker);
    expect_roundtrip(violation);

    std::string broken = "i >= 0";
    broken.push_back(static_cast<char>(0xFF));
    Invariant bad{"loop_invariant", Location{"t.c", 2, 1, "main"}, broken};
    auto kept = build_correctness_witness({bad}, meta);
    const std::string ky = to_yaml(kept);
    expect_no_raw_break(ky);
    CHECK(ky.find(bytes_of({0xFF})) == std::string::npos);
    CHECK(ky.find("\\xff") != std::string::npos);
    CHECK(ky.find("entry_type: invariant_set") != std::string::npos);
    CHECK(ky.find("loop_invariant") != std::string::npos);
    auto back = prism::taskdef::parse_yaml(ky);
    REQUIRE_MESSAGE(back.value.has_value(), back.error);
    CHECK((*back.value)[0]["entry_type"] == "invariant_set");
    CHECK((*back.value)[0]["content"][0]["invariant"]["value"] == "i >= 0" + bytes_of({0xC3, 0xBF}));
    const auto out = d.path / "w.yml";
    CHECK(write_witness(kept, out));
    const std::string disk = slurp(out);
    CHECK(disk == ky);
    CHECK(disk.find("invariant_set") != std::string::npos);
}
