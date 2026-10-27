// Taxonomy class ids name defect classes. Pipeline statuses are not classes.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/taxonomy.hpp"

#include <set>
#include <string>

TEST_CASE("taxonomy: class ids are unique, named, and not statuses") {
    std::set<std::string> ids;
    for (const auto& c : prism::taxonomy_classes()) {
        CAPTURE(c.id);
        CAPTURE(c.name);
        CHECK(!c.id.empty());
        CHECK(!c.name.empty());
        CHECK(c.id != "PROVED");
        CHECK(c.id != "BOUNDED");
        CHECK(c.id != "CLEAN");
        CHECK(c.id != "NOTRUN");
        CHECK(ids.insert(c.id).second);
    }
}
