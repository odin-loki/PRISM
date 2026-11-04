// PRISM conformance task c23/c23_has_include_true.c: expected true (no-div0)
#if __has_include(<stddef.h>)
#define DEN 2
#else
#define DEN 0
#endif
#if __has_include(<prism_no_such_header.h>)
#define DEN2 1
#else
#define DEN2 0
#endif
int c23_has_include_true(int x) {
    return x / DEN;
}
