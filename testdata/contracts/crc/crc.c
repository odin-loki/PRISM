/* Checksum loops: CRC-32 (reflected, polynomial 0xEDB88320) bit by bit and
 * with a 16-entry nibble table, the Internet checksum fold, and Fletcher-16.
 * The value contracts: the two CRC forms agree on every byte and every
 * running value; the folds stay in their ranges. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
#define ND_U8() ((unsigned char)__VERIFIER_nondet_int())
#define FILL_U8(b, cap) do { for (int i_ = 0; i_ < (int)(cap); i_++) (b)[i_] = ND_U8(); } while (0)
/* --- end prelude --- */

#define CN 3

/*@ assigns \nothing; */
unsigned crc32_byte_bitwise(unsigned crc, unsigned char b) {
    crc ^= b;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    return crc;
}

static const unsigned crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

/*@ assigns \nothing;
    ensures \result == crc32_byte_bitwise(crc, b);
*/
unsigned crc32_byte_table(unsigned crc, unsigned char b) {
    crc ^= b;
    crc = (crc >> 4) ^ crc32_nibble[crc & 15u];
    crc = (crc >> 4) ^ crc32_nibble[crc & 15u];
    return crc;
}

/* CRC-32 of buf[0..len-1] (init ~0, final xor ~0). */
/*@ requires \valid_read(buf + (0..len-1));
    requires 0 <= len <= 16;
    assigns \nothing;
    ensures len == 0 ==> \result == 0;
*/
unsigned crc32_buf(const unsigned char *buf, int len) {
    unsigned crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) crc = crc32_byte_table(crc, buf[i]);
    return ~crc;
}

/* Fold a 32-bit ones-complement sum to 16 bits and complement it. */
/*@ assigns \nothing;
    ensures \result <= 0xffff;
*/
unsigned csum_fold(unsigned sum) {
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return ~sum & 0xffff;
}

/* Internet checksum of n 16-bit words (RFC 1071). */
/*@ requires \valid_read(w + (0..n-1));
    requires 0 <= n <= 16;
    assigns \nothing;
    ensures \result <= 0xffff;
*/
unsigned inet_csum(const unsigned short *w, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; i++) sum += w[i]; /* n <= 16: no 32-bit overflow */
    return csum_fold(sum);
}

/* Fletcher-16 of buf[0..len-1]. */
/*@ requires \valid_read(buf + (0..len-1));
    requires 0 <= len <= 16;
    assigns \nothing;
    ensures (\result & 0xff) < 255 && (\result >> 8) < 255;
*/
unsigned fletcher16(const unsigned char *buf, int len) {
    unsigned s1 = 0, s2 = 0;
    for (int i = 0; i < len; i++) {
        s1 = (s1 + buf[i]) % 255;
        s2 = (s2 + s1) % 255;
    }
    return (s2 << 8) | s1;
}

/* A sum with its own checksum folds to 0 (the receiver's check). */
void inet_csum_contract(unsigned short a, unsigned short b) {
    unsigned short w[3] = {a, b, 0};
    w[2] = (unsigned short)inet_csum(w, 2);
    unsigned v = inet_csum(w, 3);
    assert(v == 0 || v == 0xffff);
}

/* Known value: CRC-32 of "123456789" is 0xCBF43926. */
void crc32_check_value_contract(void) {
    unsigned char s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    assert(crc32_buf(s, 9) == 0xCBF43926u);
}

/* Planted twin of a contract: "the CRC of any byte is non-zero" (CRC(0, 0)
 * is 0). One 8-step loop: the earlier form compared with a second call
 * (two loops) and got BOUNDED within the unwind instead of the refutation. */
/*@ ensures \result != 0; */
unsigned crc32_injective_bad(unsigned char b) { return crc32_byte_bitwise(0, b); }
