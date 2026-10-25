/* Interval operator plants. A hit is FAILED/FINDS, never a proof. */

int mod_param(int x, int y)
{
    return x % y;
}

int intmin_div(int x)
{
    return x / -1;
}

int shift_wide(int x)
{
    return x >> 32;
}

unsigned wrap_u_local(void)
{
    unsigned x;
    x = 2000000000;
    return x + 2000000000;
}

unsigned wrap_u_branch(unsigned x)
{
    if (x > 10) {
        x = 2000000000;
        return x + 2000000000;
    }
    return 0;
}
