/* PIR tasks: constructs PIR does not model are named, never dropped. */
int deref_ptr(int *p) { return *p; }
int local_array(int i) {
    int a[4] = {0, 1, 2, 3};
    return a[i & 3];
}
double twice_double(double x) { return x * 2.0; }
int recurse(int n) { return n <= 0 ? 0 : recurse(n - 1); }
