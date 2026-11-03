// PRISM conformance task c23/c23_elifdef_true.c: expected true (no-div0)
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
int c23_elifdef_true(int x) {
    return x / DEN;
}
