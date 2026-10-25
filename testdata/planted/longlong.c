/* 64-bit signed overflow is UB; 32-bit INT_MAX+INT_MAX is not. */

long long add_ll(long long a, long long b)
{
    return a + b;
}

long long add_ll_ok(long long a)
{
    if (a < 0)
        return 0;
    if (a > 10)
        return 10;
    return a + 1;
}
