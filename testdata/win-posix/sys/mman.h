/* Windows-only astlint fixture stand-in (see ../README.md). */
#ifndef PRISM_WIN_POSIX_SYS_MMAN_H
#define PRISM_WIN_POSIX_SYS_MMAN_H
#include <stddef.h>
typedef long long off_t_prism;
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t_prism off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
#endif
