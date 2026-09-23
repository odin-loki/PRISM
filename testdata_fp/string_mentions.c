/* Correct: dangerous API names appear only inside string literals. */
#include <stdio.h>

void string_mentions(void)
{
    puts("do not call gets(b) here");
    puts("strtok(s, \",\") keeps a static cursor; use strtok_r");
    puts("system(\"rm -rf /\") is what we never run");
    puts("mktemp(t) and tmpnam(0) are racy");
    puts("free(p); p[0] = 1; would be a use after free");
    puts("pthread_mutex_unlock(&m); pthread_mutex_unlock(&m);");
    puts("signal(SIGINT, h) -- prefer sigaction");
    puts("chroot(\"/jail\") without chdir(\"/\")");
}

const char *usage(void)
{
    return "usage: tool [-x] -- see sprintf(buf, fmt) and strcpy(dst, src)";
}
