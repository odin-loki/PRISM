/* Linux kernel netlink attribute, Ethernet address and DMA error specs
 * (docs/KERNEL_MODELS.md), W1 more.
 *
 * Reference semantics (include/uapi/linux/netlink.h, include/net/netlink.h,
 * include/linux/etherdevice.h, include/linux/dma-mapping.h; Linux 6.18):
 *   nla_len(nla): (u16)(nla->nla_len - NLA_HDRLEN): the payload length; reads the __u16 at offset 0.
 *   nla_data(nla): (char *)nla + NLA_HDRLEN. No read.
 *   nla_get_u32(nla): the u32 stored at (char *)nla + NLA_HDRLEN (native byte order).
 *   nla_total_size(payload): NLA_ALIGN(NLA_HDRLEN + payload) = (payload + 4 + 3) & ~3 (an int;
 *     computed here in unsigned arithmetic, so it wraps instead of being undefined).
 *   ether_addr_equal(a, b): true when the six bytes of a and b are equal; reads both.
 *   ether_addr_copy(dst, src): copies six bytes from src to dst (the real function requires u16
 *     alignment of both unless CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS).
 *   dma_mapping_error(dev, addr): -ENOMEM (-12) when addr == DMA_MAPPING_ERROR (~(dma_addr_t)0),
 *     else 0.
 * requires: nla points to a validated attribute with the bytes read here (nla_len: 2, nla_get_u32:
 *   8 bytes: the caller checks nla_len(nla) >= 4); the Ethernet addresses point to six bytes.
 * Assumptions introduced (named in the verdict):
 *   - struct nlattr is { __u16 nla_len; __u16 nla_type; } and NLA_HDRLEN is 4 (uapi layout)
 *   - the length of the attribute is validated by the caller, not by the accessor
 *   - ether_addr_copy / ether_addr_equal: alignment is not checked
 *   - dma_mapping_error: CONFIG_HAS_DMA=y (without it the function always returns -ENOMEM) and
 *     dma_addr_t is 64 bits wide (CONFIG_ARCH_DMA_ADDR_T_64BIT); DMA-API debugging has no effect
 */
#include "kernel_model.h"

extern "C" {

/* include/net/netlink.h: static inline u16 nla_len(const struct nlattr *nla) */
unsigned short nla_len(const void *nla) {
    ASSUME("linux-w1-nla_len: returns (u16)(nla->nla_len - 4), reading the __u16 at offset 0 (NLA_HDRLEN 4; struct nlattr is two __u16, uapi layout, Linux 6.18); a nla_len below 4 wraps as in the real function; nla must point to at least 2 readable bytes");
    unsigned short l = *(const unsigned short *)nla;
    return (unsigned short)(l - 4);
}

/* include/net/netlink.h: static inline void *nla_data(const struct nlattr *nla) */
void *nla_data(const void *nla) {
    ASSUME("linux-w1-nla_data: returns (char *)nla + 4 (NLA_HDRLEN; struct nlattr is two __u16, uapi layout, Linux 6.18); no read, the payload is not checked here");
    return (char *)nla + 4;
}

/* include/net/netlink.h: static inline u32 nla_get_u32(const struct nlattr *nla) */
unsigned int nla_get_u32(const void *nla) {
    ASSUME("linux-w1-nla_get_u32: returns the u32 at (char *)nla + 4 in native byte order (NLA_HDRLEN 4; struct nlattr is two __u16, uapi layout, Linux 6.18); the caller must have validated the attribute (nla_len(nla) >= 4), the model reads unconditionally like the real function; alignment is not checked");
    unsigned int v;
    __prism_memcpy(&v, (const char *)nla + 4, 4, 0);
    return v;
}

/* include/net/netlink.h: static inline int nla_total_size(int payload) */
int nla_total_size(int payload) {
    ASSUME("linux-w1-nla_total_size: returns NLA_ALIGN(NLA_HDRLEN + payload) = (payload + 4 + 3) & ~3 (include/uapi/linux/netlink.h, include/net/netlink.h, Linux 6.18); computed in unsigned arithmetic, so a payload near INT_MAX wraps instead of being undefined");
    unsigned int t = (unsigned int)payload + 4u + 3u;
    return (int)(t & ~3u);
}

/* include/linux/etherdevice.h: static inline bool ether_addr_equal(const u8 *addr1, const u8 *addr2) */
bool ether_addr_equal(const unsigned char *a, const unsigned char *b) {
    ASSUME("linux-w1-ether_addr_equal: returns true when the six bytes of both addresses are equal (include/linux/etherdevice.h, Linux 6.18); both must point to six readable bytes; the u16 / u32 alignment of the real reads is not checked");
    __prism_read_range(a, 6);
    __prism_read_range(b, 6);
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
}

/* include/linux/etherdevice.h: static inline void ether_addr_copy(u8 *dst, const u8 *src) */
void ether_addr_copy(unsigned char *dst, const unsigned char *src) {
    ASSUME("linux-w1-ether_addr_copy: copies six bytes from src to dst (include/linux/etherdevice.h, Linux 6.18); src must be readable and dst writable for six bytes; the u16 alignment the real function requires is not checked");
    __prism_memcpy(dst, src, 6, 0);
}

/* include/linux/dma-mapping.h: static inline int dma_mapping_error(struct device *dev, dma_addr_t dma_addr) */
int dma_mapping_error(void *dev, unsigned long long dma_addr) {
    ASSUME("linux-w1-dma_mapping_error: returns -ENOMEM (-12) when dma_addr == DMA_MAPPING_ERROR (~0), else 0 (include/linux/dma-mapping.h, Linux 6.18); assumes CONFIG_HAS_DMA=y (without it the real function always returns -ENOMEM) and a 64-bit dma_addr_t; dev is not read");
    (void)dev;
    return dma_addr == ~0ULL ? -12 : 0;
}

} /* extern "C" */
