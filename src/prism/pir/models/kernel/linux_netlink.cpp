/* Linux kernel API specs (docs/KERNEL_MODELS.md), W1 group g6: netlink attribute
 * payload, ceph buffer check, Ethernet address validity.
 *
 * Reference semantics (include/uapi/linux/netlink.h, include/net/netlink.h,
 * include/linux/ceph/decode.h, include/linux/etherdevice.h; Linux 6.18):
 *   nla_get_u8(nla): returns the payload byte at (char *)nla + NLA_HDRLEN (4: struct
 *     nlattr is two __u16, a uapi layout). The model reads unconditionally: the caller
 *     must have validated the attribute (nla_len >= 1, e.g. via nla_ok / nla_parse).
 *     No failure path.
 *   ceph_has_room(p, end, n): returns n <= (size_t)(end - *p). It reads *p and writes
 *     nothing (the caller advances *p itself). No failure path.
 *   is_valid_ether_addr(addr): !is_multicast_ether_addr(addr) && !is_zero_ether_addr(addr).
 *     addr[0] is read first; when it has the multicast bit (bit 0) set the result is
 *     false and addr[1..5] are not read. No failure path.
 * Assumptions introduced (named in the verdict):
 *   - struct nlattr is { __u16 nla_len; __u16 nla_type; } and NLA_HDRLEN is 4
 *     (uapi layout, no config option changes it)
 *   - the netlink attribute length is validated by the caller, not by nla_get_u8
 *   - ceph_has_room: *p and end point into the same buffer (a difference of unrelated
 *     pointers is undefined, as in the real function); a negative difference wraps
 */
#include "kernel_model.h"

extern "C" {

/* include/net/netlink.h: static inline u8 nla_get_u8(const struct nlattr *nla) */
unsigned char nla_get_u8(const void *nla) {
    ASSUME("linux-g6-nla_get_u8: returns the payload byte at (char *)nla + 4 (NLA_HDRLEN; struct nlattr is two __u16); the caller must have validated the attribute (nla_len(nla) >= 1), the model reads unconditionally like the real function");
    return *(const unsigned char *)((const char *)nla + 4);
}

/* include/linux/ceph/decode.h: static inline bool ceph_has_room(void **p, void *end, size_t n) */
bool ceph_has_room(void **p, void *end, size_t n) {
    ASSUME("linux-g6-ceph_has_room: returns n <= (size_t)(end - *p); reads *p and writes nothing, the caller advances *p itself; p must be valid and *p and end must point into the same buffer");
    void *cur = *p;
    return n <= (size_t)((char *)end - (char *)cur);
}

/* include/linux/etherdevice.h: static inline bool is_valid_ether_addr(const u8 *addr) */
bool is_valid_ether_addr(const unsigned char *addr) {
    ASSUME("linux-g6-is_valid_ether_addr: returns false when addr[0] has the multicast bit (bit 0) set (addr[1..5] are then not read), else false when all six bytes are zero, else true; a unicast addr must point to 6 readable bytes");
    if (addr[0] & 1) return false;
    if (!(addr[0] | addr[1] | addr[2] | addr[3] | addr[4] | addr[5])) return false;
    return true;
}

} /* extern "C" */
