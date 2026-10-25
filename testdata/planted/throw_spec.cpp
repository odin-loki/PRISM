void throw_spec_bad() throw() {
}

void throw_spec_dynamic_bad() throw(int) {
}

void throw_spec_ok() noexcept {
}

void throw_stmt_ok() {
    throw 1;
}
