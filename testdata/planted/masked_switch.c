enum { A = 1, B = 2 };

int masked_switch(int flags) {
    int d;
    switch (flags & (A | B)) {
    case 0: d = 0; break;
    case A: d = 1; break;
    case B: d = 2; break;
    }
    return d;
}
