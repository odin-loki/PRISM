int pthread_attr_init(void *attr);

void pattr_bad(void) {
    pthread_attr_init(0);
}

void pattr_ok(void) {
    if (pthread_attr_init(0) != 0)
        return;
}
