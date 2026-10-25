/* A K&R definition whose head no pattern reads (a lowercase macro between
 * the return type and `*name`, a variant of zlib gzlib.c gz_strwinerror):
 * it must stay a PARSE-GAP (Law 7), never a body skipped without a word.
 * testdata_tp/knr_gap.c holds the ALL_CAPS shape, which is parsed. */
typedef unsigned long DWORD;
#define zlib_internal

char zlib_internal *strwinerror(error)
    DWORD error;
{
    static char buf[8];
    buf[0] = (char)error;
    return buf;
}

int after_gap(int q) {
    return q;
}
