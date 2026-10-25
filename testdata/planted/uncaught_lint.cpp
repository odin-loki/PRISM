struct U {
    ~U() { if (std::uncaught_exceptions()) throw 1; }
};
void uncaught_bad() { if (std::uncaught_exceptions()) throw 1; }
void uncaught_ok() {
    int n = std::uncaught_exceptions();
    (void)n;
}
