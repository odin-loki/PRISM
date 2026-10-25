/* Signed add overflows for large x. BMC should FAILED with a cex. */
int add_overflow(int x) {
    return x + 100;
}
