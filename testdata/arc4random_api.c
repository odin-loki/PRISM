unsigned arc4random(void);
void arc4random_buf(void *buf, unsigned n);
unsigned arc4random_uniform(unsigned upper);

void arc4_bad(void) {
    arc4random();
}

void arc4_ok(void) {
    if (arc4random() != 0)
        return;
}
