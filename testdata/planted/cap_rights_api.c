int cap_rights_limit(int fd, const void *rights);
int cap_rights_get(int fd, void *rights);

void capr_bad(void) {
    cap_rights_limit(0, 0);
}

void capr_ok(void) {
    if (cap_rights_limit(0, 0) != 0)
        return;
}
