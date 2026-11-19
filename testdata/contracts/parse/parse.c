/* Parsers over byte buffers: a TLV walk (type, length, value records) and
 * an unsigned LEB128 varint decoder. Every read stays below len. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
#define ND_U8() ((unsigned char)__VERIFIER_nondet_int())
#define FILL_U8(b, cap) do { for (int i_ = 0; i_ < (int)(cap); i_++) (b)[i_] = ND_U8(); } while (0)
/* --- end prelude --- */

#define PN 6
/* The walks refuse messages longer than TLV_MAX_LEN bytes: the bound makes
 * both stages close the loop by unwinding (without it the walk loops did
 * not close: BOUNDED after 300 s, docs/CONTRACTS.md gap list). */
#define TLV_MAX_LEN 8

/* Number of whole records in buf[0..len-1], or -1 when a record runs past
 * the end. A record is: type (1 byte), length L (1 byte), L value bytes. */
/*@ requires \valid_read(buf + (0..len-1));
    requires 0 <= len;
    assigns \nothing;
    ensures -1 <= \result <= len / 2;
*/
int tlv_count(const unsigned char *buf, int len) {
    if (len > TLV_MAX_LEN) return -1; /* longer messages are refused */
    int off = 0, n = 0;
    while (off < len) {
        if (len - off < 2) return -1;
        int l = buf[off + 1];
        if (l > len - off - 2) return -1;
        off += 2 + l;
        n++;
    }
    return n;
}

/* Find the value of the first record of type t: its offset, or -1. */
/*@ requires \valid_read(buf + (0..len-1)) && \valid(vlen);
    requires 0 <= len;
    assigns *vlen;
    ensures -1 <= \result <= len;
    ensures \result >= 0 ==> 0 <= *vlen && \result + *vlen <= len && buf[\result - 2] == t;
*/
int tlv_find(const unsigned char *buf, int len, unsigned char t, int *vlen) {
    if (len > TLV_MAX_LEN) return -1;
    int off = 0;
    while (off + 2 <= len) {
        int l = buf[off + 1];
        if (l > len - off - 2) return -1;
        if (buf[off] == t) {
            *vlen = l;
            return off + 2;
        }
        off += 2 + l;
    }
    return -1;
}

/* Unsigned LEB128 into 32 bits: bytes consumed (1..5), or -1 when the
 * buffer ends first or the value does not fit. */
/*@ requires \valid_read(buf + (0..len-1)) && \valid(out);
    requires 0 <= len <= 16;
    assigns *out;
    ensures \result == -1 || (1 <= \result <= 5 && \result <= len);
    ensures \result == 1 ==> *out == buf[0];
*/
int uleb128_u32(const unsigned char *buf, int len, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 5; i++) {
        if (i >= len) return -1;
        unsigned char b = buf[i];
        if (i == 4 && (b & 0xf0)) return -1; /* more than 32 bits */
        v |= (unsigned)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            *out = v;
            return i + 1;
        }
    }
    return -1;
}

void tlv_count_contract(int len) {
    unsigned char b[PN];
    FILL_U8(b, PN);
    if (len < 0 || len > PN) return;
    int n = tlv_count(b, len);
    assert(n >= -1 && n <= len / 2);
    if (len == 0) assert(n == 0);
}

void tlv_find_contract(int len, unsigned char t) {
    unsigned char b[PN];
    int vl = -1;
    FILL_U8(b, PN);
    if (len < 0 || len > PN) return;
    int r = tlv_find(b, len, t, &vl);
    if (r >= 0) {
        assert(r >= 2 && b[r - 2] == t && vl == b[r - 1] && r + vl <= len);
    }
}

/* encode then decode round trip for values below 2^14 (two bytes) */
void uleb128_contract(unsigned v) {
    unsigned char b[3];
    unsigned out = 0;
    if (v >= (1u << 14)) return;
    int n;
    if (v < 128) {
        b[0] = (unsigned char)v;
        n = 1;
    } else {
        b[0] = (unsigned char)(0x80 | (v & 0x7f));
        b[1] = (unsigned char)(v >> 7);
        n = 2;
    }
    b[n] = 0;
    assert(uleb128_u32(b, n, &out) == n && out == v);
}

/* Planted twin: the length byte trusted, the value read past the buffer. */
int tlv_trust_len_bad(int len) {
    unsigned char b[PN];
    FILL_U8(b, PN);
    if (len < 2 || len > PN) return 0;
    int l = b[1];
    int s = 0;
    for (int i = 0; i < l; i++) s += b[2 + i];
    return s;
}
