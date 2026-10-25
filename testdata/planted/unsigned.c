/* Unsigned compares and wrap. Signed overflow must not fire. */

unsigned add_u(unsigned a, unsigned b)
{
    return a + b;
}

int idx_u_ok(unsigned i)
{
    int a[4] = {0};
    if (i >= 4)
        return 0;
    return a[i];
}

int idx_u_bad(unsigned i)
{
    int a[4];
    return a[i];
}

int dead_u(unsigned n)
{
    if (n < 0)
        return 0;
    return 1;
}
