/* long and size_t are 64 bits (LP64): none of this overflows. The interval
   and fuzz stages used to read long as a 32-bit int and report
   INT-SIGNED-OVF here. The 64-bit twins are in testdata_tp/long_arith.c. */

long scale_long(int a)
{
    long x = a;
    x = x * 100000;
    return x;
}

long long widen_mul(int a, int b)
{
    long long p = (long long)a * b;
    return p;
}

unsigned long bytes_for(unsigned n)
{
    unsigned long total = n;
    total = total * 8UL;
    return total + sizeof(long);
}

int long_is_wide(int d)
{
    if (sizeof(long) == 4)
        return 100 / d;
    return 0;
}
