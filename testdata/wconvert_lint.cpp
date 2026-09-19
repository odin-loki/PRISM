string wconv_bad(void) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
    return cv.to_bytes(L"x");
}
string wconv_ok(void) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
    auto s = cv.to_bytes(L"x");
    if (s.empty()) return {};
    return s;
}
