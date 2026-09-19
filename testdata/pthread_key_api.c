int pthread_key_create(void *key, void *destructor);
int pthread_setspecific(unsigned key, const void *value);

void tkey_bad(void) {
    pthread_key_create(0, 0);
}

void tkey_ok(void) {
    if (pthread_key_create(0, 0) != 0)
        return;
    if (pthread_setspecific(0, 0) != 0)
        return;
}
