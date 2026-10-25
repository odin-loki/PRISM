/* tinyexpr.c:882/896: a case arm that ends in a nested switch with a
 * default whose every arm returns does not fall through. */
int arity_of(int a, int b)
{
    switch (a) {
    case 1:
        switch (b) {
        case 0: return 1;
        default: return 2;
        }
    case 2:
        switch (b) {
        case 0:
        case 1:
            return 3;
        default:
            return 4;
        }
    default:
        return 0;
    }
}
