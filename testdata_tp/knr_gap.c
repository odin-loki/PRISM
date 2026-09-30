/* zlib gzlib.c gz_strwinerror shape: a K&R definition with a macro between
 * the return type and `*name`. The C++ engine parses it; the Python engine
 * reported it as a PARSE-GAP (Law 7), docs/EVALUATION.md. */
typedef unsigned long DWORD;
#define ZLIB_INTERNAL

char ZLIB_INTERNAL *strwinerror(error)
    DWORD error;
{
    static char buf[8];
    buf[0] = (char)error;
    return buf;
}

int after_gap(int q) {
    return q;
}
