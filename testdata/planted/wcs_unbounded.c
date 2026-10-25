typedef int wchar_t;
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, unsigned long n);

void wcs_copy_bad(void) {
    wchar_t b[4];
    wcscpy(b, L"abcdefgh");
}

void wcs_copy_ok(void) {
    wchar_t b[8];
    wcsncpy(b, L"ab", 7);
}
