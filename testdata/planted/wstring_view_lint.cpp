wstring_view wv_bad(void) {
    std::wstring s=L"x";
    std::wstring_view v=s;
    return v;
}

int wv_ok(void) {
    std::wstring s=L"x";
    std::wstring_view v=s;
    return (int)v.size();
}
