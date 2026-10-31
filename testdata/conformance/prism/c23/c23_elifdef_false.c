// PRISM conformance task c23/c23_elifdef_false.c: expected false (no-div0)
#ifdef PRISM_UNDEFINED_MACRO
#define DEN 0
#elifdef __STDC__
#define DEN 4
#else
#define DEN 0
#endif
#ifndef PRISM_UNDEFINED_MACRO
#define DEN2 0
#elifndef __STDC__
#define DEN2 1
#endif
int c23_elifdef_false(int x) {
    return x / (DEN2 + (x & 0));
}
