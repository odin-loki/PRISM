/* Reduced from jsmn (MIT, jsmn.h jsmn_parse_primitive) and tinyexpr
 * (zlib licence, tinyexpr.c te_free_parameters), docs/EVALUATION.md.
 * Stacked labels split by #ifdef, char-literal labels ':' and '}', GCC
 * "Falls through." comments, and a last arm that leaves the switch: none
 * of these is an unannotated fallthrough. */
struct parser { unsigned pos; };

void te_free(void *p);

int parse_primitive(struct parser *parser, const char *js, unsigned len) {
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        switch (js[parser->pos]) {
#ifndef JSMN_STRICT
        case ':':
#endif
        case '\t':
        case '\n':
        case ',':
        case ']':
        case '}':
            goto found;
        default:
            break;
        }
    }
    return -1;
found:
    return (int)parser->pos;
}

struct expr { int type; void *parameters[4]; };

void *find_builtin(const char *name);

int lookup_type(const char *name, struct expr *var) {
    if (!var) var = find_builtin(name);
    if (!var) {
        return -1;
    } else {
        return var->type;
    }
}

void free_parameters(struct expr *n) {
    if (!n) return;
    switch (n->type) {
        case 4: case 14: te_free(n->parameters[3]);     /* Falls through. */
        case 3: case 13: te_free(n->parameters[2]);     /* Falls through. */
        case 2: case 12: te_free(n->parameters[1]);     /* Falls through. */
        case 1: case 11: te_free(n->parameters[0]);
    }
}

int last_arm_leaves(int k) {
    int r = 0;
    switch (k) {
    case 0:
        r = 1;
        break;
    default:
        r = 2;
    }
    return r;
}

/* tinyexpr: clock() is no lock, a char-literal '&' is no operator, and a
 * local assigned through its address is not read uninitialised. */
long clock(void);
double te_interp(const char *expr, int *error);

long timed(void) {
    long start = clock();
    long stop = clock();
    return stop - start;
}

int infix(const char *next) {
    int t = 0;
    switch (next[0]) {
    case '&':
        if (next[1] == '&') t = 1;
        break;
    case '|':
        if (next[1] == '|') t = 2;
        break;
    default:
        break;
    }
    return t;
}

int checked(const char *expr) {
    int err;
    double r = te_interp(expr, &err);
    if (err) return -1;
    return (int)r;
}

/* tinyexpr parse_number: the <ctype.h> argument cast. */
int isxdigit(int c);
int hex_prefix(const char *p) {
    return p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2]);
}
