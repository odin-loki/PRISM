/* Correct K&R definitions: parsed as functions, nothing to report. */
int knr_add(a, b)
    int a;
    int b;
{
    return a + b;
}

int knr_first(s)
    const char *s;
{
    return s != 0 ? s[0] : 0;
}
