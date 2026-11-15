// bare-metal AArch64 entry for the QEMU 'virt' machine
.section .text.boot
.global _start
_start:
    ldr x0, =0x40100000       // stack
    mov sp, x0
    bl  guest_main
1:  wfi
    b   1b
