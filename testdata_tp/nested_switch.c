/* A nested switch whose arm breaks, or that has no default, leaves only
 * itself: the outer arm still falls into the next label. */
int arity_break(int a, int b)
{
    int r = 0;
    switch (a) {
    case 1:
        switch (b) {
        case 0: r = 1; break;
        default: return 2;
        }
    case 2:
        r += 2;
        break;
    default:
        return 0;
    }
    return r;
}

int arity_no_default(int a, int b)
{
    switch (a) {
    case 1:
        switch (b) {
        case 0: return 1;
        case 1: return 2;
        }
    case 2:
        return 3;
    default:
        return 0;
    }
}
