/* PRISM hard benchmark: TLV / packet parsers over byte buffers with
 * \valid contracts. See README.md. */
#include <stdint.h>

/* Walk type-length-value records; every read is checked against n. */
// requires: \valid_read(buf + (0..n-1))
int tlv_walk_ok(const unsigned char *buf, int n) {
    if (n < 0 || n > 32) return -1;
    int off = 0, recs = 0, sum = 0;
    while (off + 2 <= n) {
        int t = buf[off], l = buf[off + 1];
        if (off + 2 + l > n) return -2;
        for (int i = 0; i < l; i++) sum += buf[off + 2 + i] ^ t;
        off += 2 + l;
        recs++;
    }
    return recs * 1000 + (sum & 0x3ff);
}

/* The length check forgets the 2-byte header: reads past the end. */
// requires: \valid_read(buf + (0..n-1))
int tlv_walk_bad(const unsigned char *buf, int n) {
    if (n < 0 || n > 16) return -1;
    int off = 0, sum = 0;
    while (off + 2 <= n) {
        int l = buf[off + 1];
        if (off + l > n) return -2;                    /* should be off + 2 + l */
        for (int i = 0; i < l; i++) sum += buf[off + 2 + i];
        off += 2 + l;
    }
    return sum;
}

/* IPv4-like header: fixed 20-byte read, checksum over 10 16-bit words. */
// requires: \valid_read(p + (0..19))
int ip_header_ok(const unsigned char *p) {
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    int ihl = p[0] & 15;
    int tot = (p[2] << 8) | p[3];
    if (ihl < 5 || tot < ihl * 4) return -1;
    return (int)(~sum & 0xffff);
}

/* The options length is ihl*4 - 20, read from a 40-byte packet without a
 * bound on ihl past 10: ihl == 15 reads p[59]. */
// requires: \valid_read(p + (0..39))
int ip_options_bad(const unsigned char *p) {
    int ihl = p[0] & 15;
    if (ihl < 5) return -1;
    int s = 0;
    for (int i = 20; i < ihl * 4; i++) s += p[i];
    return s;
}

/* Copy fields out of a packet into a caller struct. */
struct hdr { uint16_t sport, dport; uint32_t seq; uint8_t flags; };
// requires: \valid_read(p + (0..n-1))
// requires: \valid(h)
int parse_hdr_ok(const unsigned char *p, int n, struct hdr *h) {
    if (n < 13 || n > 64) return -1;
    h->sport = (uint16_t)((p[0] << 8) | p[1]);
    h->dport = (uint16_t)((p[2] << 8) | p[3]);
    h->seq = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    h->flags = p[12];
    return h->sport == 0 ? -1 : 0;
}

/* Planted bug: the shift of p[4] is done in int (p[4] << 24 overflows when
 * p[4] >= 128). */
// requires: \valid_read(p + (0..7))
int parse_seq_bad(const unsigned char *p) {
    int seq = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    return seq;
}

/* Varint (LEB128) decoder, up to 10 bytes, into 64 bits. */
// requires: \valid_read(p + (0..n-1))
uint64_t varint_ok(const unsigned char *p, int n) {
    if (n < 0 || n > 10) return 0;
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        v |= (uint64_t)(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) break;
    }
    return v;
}

/* Varint with no length limit on the shift: 7*i reaches 70 (shift >= 64). */
// requires: \valid_read(p + (0..n-1))
uint64_t varint_bad(const unsigned char *p, int n) {
    if (n < 0 || n > 11) return 0;
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        v |= (uint64_t)(p[i] & 0x7f) << (7 * i);   /* i == 10: shift by 70 */
        if (!(p[i] & 0x80)) break;
    }
    return v;
}

/* Record count divides the payload: zero records is checked. */
// requires: \valid_read(p + (0..n-1))
int avg_record_ok(const unsigned char *p, int n) {
    if (n < 1 || n > 32) return 0;
    int cnt = p[0], s = 0;
    for (int i = 1; i < n; i++) s += p[i];
    return cnt ? s / cnt : 0;
}

/* Planted division by zero: the record count byte can be 0. */
// requires: \valid_read(p + (0..n-1))
int avg_record_bad(const unsigned char *p, int n) {
    if (n < 1 || n > 32) return 0;
    int cnt = p[0], s = 0;
    for (int i = 1; i < n; i++) s += p[i];
    return s / cnt;
}
