// std::any operational model (src/prism/pir/models/cxx/any) against C++23
// [any]. The pir stage puts the model headers first on the include path, so
// <any> below is the model (the report's extra.cxx_models says "model: any").
// Each function is one contract; _false ones must fail for the class their
// yml names. make_any, pointer any_cast, type(), and comparison are not
// part of the model.
#include <any>
#include <cassert>

// The int that was stored is the int any_cast returns. reset drops it.
int any_roundtrip_true(int x) {
    std::any a(x);
    assert(a.has_value());
    assert(std::any_cast<int>(a) == x);
    std::any b(a);
    assert(std::any_cast<int>(b) == x);
    a.reset();
    assert(!a.has_value());
    assert(std::any_cast<int>(b) == x);
    return 0;
}

// A move leaves the source empty and does not free the moved-to object.
int any_move_true(int x) {
    std::any a(x);
    std::any b(std::move(a));
    assert(!a.has_value());
    assert(std::any_cast<int>(b) == x);
    std::any c(0);
    c = std::move(b);
    assert(!b.has_value());
    assert(std::any_cast<int>(c) == x);
    return 0;
}

// any_cast on an empty any is a failed precondition.
int any_empty_cast_false(int c) {
    std::any a;
    if (c == 0) a.reset();
    return std::any_cast<int>(a);
}

// any_cast of a different type is a failed precondition, not the other
// object's bytes read as int.
int any_wrong_type_false(int c) {
    std::any a(c ? 1.5 : 2.5);
    return std::any_cast<int>(a);
}
