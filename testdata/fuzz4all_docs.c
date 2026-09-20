/* Fuzz4All documentation: clamp x into [0, 100] for seed distillation. */
int fuzz4all_docs(int x) {
    // usage: pass a signed integer; values outside the range saturate
    if (x > 100) return 100;
    if (x < 0) return 0;
    return x;
}
