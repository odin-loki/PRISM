/* Linux MMIO specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (include/asm-generic/iomap.h, lib/iomap.c,
 * include/asm-generic/io.h, arch/x86/include/asm/io.h; Linux 6.18):
 *   ioread8/16/32, readb/w/l/q (when declared out of line): read one register of
 *   the stated width from device space: an arbitrary value of that width.
 *   iowrite8/16/32, writeb/w/l/q: write one register: no effect on any memory the
 *   analysed function can see.
 *   ioremap(offset, size): NULL, or a mapping; the mapped registers are device
 *   space, not program memory, so the pointer is a fresh object of `size` bytes of
 *   arbitrary initialised bytes (a plain dereference stays in bounds checked; the
 *   register accessors above never touch it).
 * Assumptions introduced (named in the verdict):
 *   - the register space is the environment: every read returns an arbitrary value
 *     of its width (no device model, no read-to-clear, no ordering between
 *     writes and reads); this is the untrusted-device assumption
 *   - writes have no effect on program memory (DMA triggered by a register write
 *     is not modelled)
 *   - the accessed address is not checked (the register space and its size are
 *     the device's, not the model's)
 * The x86 inline versions of readl/writel (inline asm) are covered by asm-havoc
 * (docs/KERNEL_MODELS.md), not by these specs.
 */
#include "kernel_model.h"

extern "C" {

#define MMIO_NOTE "linux-mmio: the register space is the environment: reads return an arbitrary value of their width, writes have no effect on program memory; the address is not checked; no device model (untrusted device)"

unsigned int ioread8(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_uchar(); }
unsigned int ioread16(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_ushort(); }
unsigned int ioread32(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_uint(); }
void iowrite8(unsigned char v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }
void iowrite16(unsigned short v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }
void iowrite32(unsigned int v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }

unsigned char readb(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_uchar(); }
unsigned short readw(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_ushort(); }
unsigned int readl(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_uint(); }
unsigned long readq(const void *addr) { (void)addr; ASSUME(MMIO_NOTE); return __VERIFIER_nondet_ulong(); }
void writeb(unsigned char v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }
void writew(unsigned short v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }
void writel(unsigned int v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }
void writeq(unsigned long v, void *addr) { (void)v; (void)addr; ASSUME(MMIO_NOTE); }

/* io.h: void __iomem *ioremap(resource_size_t offset, unsigned long size): NULL or a mapping of `size` bytes */
void *ioremap(unsigned long offset, unsigned long size) {
    (void)offset;
    ASSUME(MMIO_NOTE);
    if (size >= PRISM_MAX_OBJ || CHOICE()) return 0;
    return __prism_alloc(size, PRISM_EXTERN, PRISM_HAVOC);
}
void iounmap(void *addr) { (void)addr; ASSUME(MMIO_NOTE); }

}  // extern "C"
