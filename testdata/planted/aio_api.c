int aio_read(void *aiocbp);
int aio_write(void *aiocbp);

void aio_bad(void) {
    aio_read(0);
}

void aio_ok(void) {
    if (aio_read(0)!=0)
        return;
}
