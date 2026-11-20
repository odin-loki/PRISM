#!/bin/bash
# select_candidates.sh LINUX_TREE OSPROVING_DATA_CLANG_LINUX_DIR
# Candidate small MMIO drivers for the module-layer prototype (docs/DEVICE_MODEL.md section 9.1).
# Criteria, all by script: 80..420 non-blank lines; at least 3 MMIO accesses (readl/writel/ioread/iowrite);
# a request_irq call; .probe and .remove in a registration table; an ioremap; and the ops/registration table
# types the OS-Proving clang pass found for the file (data/clang/linux/ops.tsv.gz: table_type column).
tree=${1:-/home/odin/pbsd-ref/linux}; ops=${2:-/mnt/c/ParanoidBSD/OS-Proving/data/clang/linux}/ops.tsv.gz
for d in drivers/rtc drivers/watchdog drivers/char/hw_random drivers/char drivers/misc drivers/mfd drivers/input/misc \
         drivers/input/serio drivers/leds drivers/gpio drivers/clk drivers/mailbox drivers/hwspinlock drivers/dma; do
  for f in "$tree/$d"/*.c; do
    [ -f "$f" ] || continue
    n=$(grep -cv '^[[:space:]]*$' "$f")
    [ "$n" -ge 80 ] && [ "$n" -le 420 ] || continue
    mm=$(grep -cE '\b(readl|writel|readb|writeb|readw|writew|ioread[0-9]+|iowrite[0-9]+)(_relaxed)?[[:space:]]*\(' "$f")
    [ "$mm" -ge 3 ] || continue
    grep -qE 'request_irq|request_threaded_irq|devm_request_irq' "$f" || continue
    grep -qE '\.probe[[:space:]]*=' "$f" || continue
    grep -qE '\.remove(_new)?[[:space:]]*=' "$f" || continue
    grep -qE 'ioremap|devm_platform_ioremap|devm_ioremap_resource' "$f" || continue
    t=$(zcat "$ops" 2>/dev/null | awk -F'\t' -v f="$f" '$4==f {print $3}' | sort -u | tr '\n' ',' )
    printf '%5d lines %3d mmio  %s  [%s]\n' "$n" "$mm" "${f#$tree/}" "$t"
  done
done | sort -n
