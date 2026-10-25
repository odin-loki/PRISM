int crypt_newhash(const char *password, const char *pref, char *hash,
                  unsigned hashsize);

void cnew_bad(void) {
    crypt_newhash("p", "bcrypt", 0, 0);
}

void cnew_ok(void) {
    if (crypt_newhash("p", "bcrypt", 0, 0) != 0)
        return;
}
