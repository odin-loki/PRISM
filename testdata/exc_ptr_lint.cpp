void exc_ptr_bad(void) {
    std::exception_ptr e;
    std::rethrow_exception(e);
}

void exc_ptr_ok(void) {
    std::exception_ptr e;
    if (!e) return;
    std::rethrow_exception(e);
}
