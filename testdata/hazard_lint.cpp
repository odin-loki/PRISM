int hazard_bad(void) {
    std::hazard_pointer hp;
    Node *p = atom.load();
    return p->n;
}

int hazard_ok(void) {
    std::hazard_pointer hp;
    auto *p = hp.protect(atom);
    return p->n;
}
