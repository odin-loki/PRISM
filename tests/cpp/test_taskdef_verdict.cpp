// expected_verdict on a task property (src/prism/taskdef.cpp parse_taskdef).
//
// A JSON/YAML boolean stays. The strings "true" and "false" are exact.
// Any other type or text fails the load. A missing key stays unset.
// docs/finish-notes/taskdef-verdict.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/taskdef.hpp"

#include <string>

namespace {

nlohmann::ordered_json yaml_doc(const std::string& verdict_token) {
    auto y = prism::taskdef::parse_yaml(
        "format_version: '2.0'\ninput_files: t.i\nproperties:\n"
        "  - property_file: p.prp\n    expected_verdict: " +
        verdict_token + "\n");
    REQUIRE(y.value.has_value());
    return *y.value;
}

nlohmann::ordered_json json_doc(const std::string& verdict_literal) {
    return nlohmann::ordered_json::parse(
        "{\"format_version\":\"2.0\",\"input_files\":[\"t.i\"],\"properties\":["
        "{\"property_file\":\"p.prp\",\"expected_verdict\":" +
        verdict_literal + "}]}");
}

void expect_verdict(const nlohmann::ordered_json& doc, bool want) {
    INFO(doc.dump());
    std::string error;
    auto t = prism::taskdef::parse_taskdef(doc, &error);
    REQUIRE(t.has_value());
    REQUIRE(t->properties.size() == 1);
    REQUIRE(t->properties[0].expected_verdict.has_value());
    CHECK(t->properties[0].expected_verdict == want);
    CHECK(error.empty());
}

void expect_load_error(const nlohmann::ordered_json& doc) {
    INFO(doc.dump());
    std::string error;
    auto t = prism::taskdef::parse_taskdef(doc, &error);
    CHECK_FALSE(t.has_value());
    CHECK(error.find("expected_verdict") != std::string::npos);
    if (t) {
        for (const auto& p : t->properties) CHECK(p.expected_verdict != true);
    }
}

}  // namespace

TEST_CASE("taskdef: boolean expected_verdict false stays false") {
    for (const auto& doc : {yaml_doc("false"), json_doc("false")}) {
        CHECK(doc["properties"][0]["expected_verdict"].is_boolean());
        CHECK(doc["properties"][0]["expected_verdict"] == false);
        expect_verdict(doc, false);
    }
}

TEST_CASE("taskdef: boolean expected_verdict true stays true") {
    for (const auto& doc : {yaml_doc("true"), json_doc("true")}) {
        CHECK(doc["properties"][0]["expected_verdict"].is_boolean());
        CHECK(doc["properties"][0]["expected_verdict"] == true);
        expect_verdict(doc, true);
    }
}

TEST_CASE("taskdef: text expected_verdict false is false") {
    for (const auto& doc : {yaml_doc("\"false\""), json_doc("\"false\"")}) {
        CHECK(doc["properties"][0]["expected_verdict"].is_string());
        CHECK(doc["properties"][0]["expected_verdict"] == "false");
        expect_verdict(doc, false);
    }
}

TEST_CASE("taskdef: text expected_verdict true is true") {
    for (const auto& doc : {yaml_doc("\"true\""), json_doc("\"true\"")}) {
        CHECK(doc["properties"][0]["expected_verdict"].is_string());
        CHECK(doc["properties"][0]["expected_verdict"] == "true");
        expect_verdict(doc, true);
    }
}

TEST_CASE("taskdef: yes, FALSE, and a number are not an expected verdict") {
    for (const char* text : {"\"yes\"", "\"FALSE\"", "\"TRUE\"", "\"False\"", "\"no\"", "\"1\"", "\"0\""}) {
        INFO(text);
        expect_load_error(yaml_doc(text));
        expect_load_error(json_doc(text));
    }
    for (const char* number : {"1", "0"}) {
        INFO(number);
        const auto yaml = yaml_doc(number);
        const auto json = json_doc(number);
        CHECK(yaml["properties"][0]["expected_verdict"].is_number());
        CHECK(json["properties"][0]["expected_verdict"].is_number());
        expect_load_error(yaml);
        expect_load_error(json);
    }
    // A later bad verdict fails the load. It is not dropped, and the earlier
    // true property is not returned as the task's expected verdict.
    auto mixed = prism::taskdef::parse_yaml(
        "input_files: t.i\nproperties:\n"
        "  - property_file: a.prp\n    expected_verdict: true\n"
        "  - property_file: b.prp\n    expected_verdict: \"yes\"\n");
    REQUIRE(mixed.value.has_value());
    expect_load_error(*mixed.value);
}

TEST_CASE("taskdef: a missing expected_verdict stays unset") {
    auto y = prism::taskdef::parse_yaml(
        "input_files: t.i\nproperties:\n  - property_file: p.prp\n");
    REQUIRE(y.value.has_value());
    CHECK_FALSE(y.value->at("properties")[0].contains("expected_verdict"));
    std::string error = "unchanged";
    auto t = prism::taskdef::parse_taskdef(*y.value, &error);
    REQUIRE(t.has_value());
    REQUIRE(t->properties.size() == 1);
    CHECK_FALSE(t->properties[0].expected_verdict.has_value());
    CHECK(error == "unchanged");

    auto j = nlohmann::ordered_json::parse(
        "{\"input_files\":[\"t.i\"],\"properties\":[{\"property_file\":\"p.prp\"}]}");
    CHECK_FALSE(j["properties"][0].contains("expected_verdict"));
    error = "unchanged";
    t = prism::taskdef::parse_taskdef(j, &error);
    REQUIRE(t.has_value());
    CHECK_FALSE(t->properties[0].expected_verdict.has_value());
    CHECK(error == "unchanged");
}
