void semaphore_bad(void) {
    std::counting_semaphore sem(0);
    sem.acquire();
}

void semaphore_ok(void) {
    std::counting_semaphore sem(1);
    sem.acquire();
    sem.release();
}
