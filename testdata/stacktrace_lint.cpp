int stacktrace_bad(void) {
    std::stacktrace st = std::stacktrace::current();
    return (int)st[0];
}

int stacktrace_ok(void) {
    std::stacktrace st = std::stacktrace::current();
    if (!st.empty())
        return (int)st[0];
    return 0;
}
