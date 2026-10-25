/* PIR tasks: integer conversions (narrowing is implementation-defined, not UB). */
int trunc_ok(long x) { return (int)x; }
int widen_bad(short a, short b) { return a * b * 2; }
int widen_ok(short a, short b) { return a * b; }
unsigned char uchar_ok(unsigned char a, unsigned char b) { return a + b; }
long sext_mul_ok(int a, int b) { return (long)a * b; }
