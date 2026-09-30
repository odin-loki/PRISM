/* The 64-bit twins of testdata_fp/long_arith.c: each overflows at
   LONG_MAX / LLONG_MAX, never at INT_MAX. */

long next_long(long a)
{
    return a + 1;
}

long long square_ll(long long a)
{
    return a * a;
}

long long ull_sum(long long a, long long b)
{
    long long s = a + b;
    return s;
}
