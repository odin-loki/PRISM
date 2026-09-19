typedef struct { int _x; } pthread_mutex_t;
typedef struct { int _x; } mtx_t;
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_init(pthread_mutex_t *m, void *a);
int mtx_lock(mtx_t *m);
int mtx_init(mtx_t *m, const char *n, void *t, int f);

void lock_init_bad(void) {
    pthread_mutex_t m;
    pthread_mutex_lock(&m);
}

void mtx_init_bad(void) {
    mtx_t m;
    mtx_lock(&m);
}

void lock_init_ok(void) {
    pthread_mutex_t m;
    pthread_mutex_init(&m, 0);
    pthread_mutex_lock(&m);
}

void lock_init_static_ok(void) {
    pthread_mutex_t m = {0};
    pthread_mutex_lock(&m);
}

void lock_init_param_ok(pthread_mutex_t *m) {
    pthread_mutex_lock(m);
}
