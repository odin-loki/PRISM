/* PRISM device-model harness, Linux flavour: included BEFORE the kernel headers.
 *
 * asm-generic/bug.h defines the WARN family only `#ifndef WARN_ON` (and so on), so defining them first
 * makes the header's own inline functions (virtio_device_ready, ...) use these too. The x86 definitions
 * expand to inline asm (ud2 plus a bug table), which pir does not encode. A WARN has no effect here: it
 * evaluates its condition and yields it. */
#ifndef PRISM_LINUX_EARLY_H
#define PRISM_LINUX_EARLY_H

#define WARN_ON(condition) ({ int prism_w_ = !!(condition); prism_w_; })
#define WARN_ON_ONCE(condition) ({ int prism_w_ = !!(condition); prism_w_; })
#define WARN(condition, format...) ({ int prism_w_ = !!(condition); prism_w_; })
#define WARN_ONCE(condition, format...) ({ int prism_w_ = !!(condition); prism_w_; })

#endif
