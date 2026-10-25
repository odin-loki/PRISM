int contracts_bad(int n) {
    contract_assert(false);
    return n;
}

int contracts_ok(int n) {
    if (n <= 0)
        return 0;
    contract_assert(n > 0);
    return n;
}
