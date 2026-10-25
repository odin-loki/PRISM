void binsem_bad(void) {
    std::binary_semaphore sem(0);
    sem.acquire();
}

void binsem_ok(void) {
    std::binary_semaphore sem(1);
    sem.acquire();
}
