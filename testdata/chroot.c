#include <unistd.h>

/* chroot("/tmp"); — comment only, not a call */

void chroot_bad(void) {
    chroot("/jail");
}

void chroot_ok(void) {
    chroot("/jail");
    chdir("/");
}
