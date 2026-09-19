int variant_valueless_bad(void) {
    variant<int, Throwy> v;
    v.emplace<Throwy>();
    return std::get<Throwy>(v);
}

int variant_valueless_ok(void) {
    variant<int, Throwy> v;
    v.emplace<Throwy>();
    if (!v.valueless_by_exception())
        return std::get<Throwy>(v);
    return 0;
}
