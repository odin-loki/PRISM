// No threads: the conc stage has nothing to check and reports nothing.
int global = 0;

int bump(int x) {
    global = global + x;
    return global;
}

int main(void) { return bump(1); }
