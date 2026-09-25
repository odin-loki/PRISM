/* A K&R definition the parser cannot read (zlib gzlib.c gz_strwinerror
 * shape: a macro between the return type and `*name`) is a PARSE-GAP,
 * never a body skipped without a word (Law 7), docs/EVALUATION.md. */
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
