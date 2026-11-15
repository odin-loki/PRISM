#!/bin/bash
# Cross-check of the PL031 device model against QEMU (docs/DEVICE_MODEL.md section 7).
# usage: run.sh PRISM_MODULE_BIN PL031_PDM WORKDIR [N_TRACES] [N_OPS]
# Needs: clang with aarch64 target, ld.lld, qemu-system-aarch64, a C compiler.
set -u
PM=$1; PDM=$2; W=$3; N=${4:-12}; OPS=${5:-16}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W"
$PM device-c "$PDM" > "$W/model.c" || exit 2
cc -O1 -o "$W/gen_trace" "$HERE/gen_trace.c" || exit 2
cc -O1 -DMODEL_C="\"$W/model.c\"" -o "$W/replay" "$HERE/replay.c" || exit 2
CLANG=${CLANG:-clang}
$CLANG --target=aarch64-none-elf -ffreestanding -c -o "$W/start.o" "$HERE/start.S" || exit 2
pass=0; fail=0; reads=0
for seed in $(seq 1 "$N"); do
  "$W/gen_trace" "$seed" "$OPS" > "$W/trace$seed.inc"
  sed "s#\"TRACE\"#\"$W/trace$seed.inc\"#" "$HERE/guest_template.c" > "$W/guest$seed.c"
  cat >> "$W/guest$seed.c" <<'XEOF'
XEOF
  $CLANG --target=aarch64-none-elf -ffreestanding -O1 -fno-builtin -c -o "$W/guest$seed.o" "$W/guest$seed.c" || { echo "seed $seed: guest build failed"; fail=$((fail+1)); continue; }
  ld.lld -T "$HERE/link.ld" -o "$W/guest$seed.elf" "$W/start.o" "$W/guest$seed.o" || exit 2
  timeout 40 qemu-system-aarch64 -M virt -cpu cortex-a57 -m 64 -display none -serial stdio -monitor none -semihosting -kernel "$W/guest$seed.elf" > "$W/qemu$seed.out" 2>/dev/null
  tr -d '\r' < "$W/qemu$seed.out" | sed '/^DONE/q' > "$W/qemu$seed.trace"
  if ! grep -q '^DONE' "$W/qemu$seed.trace"; then echo "seed $seed: QEMU run did not finish"; fail=$((fail+1)); continue; fi
  out=$("$W/replay" "$W/qemu$seed.trace"); rc=$?
  echo "seed $seed: $(echo "$out" | tail -1)"
  [ $rc -eq 0 ] && pass=$((pass+1)) || { fail=$((fail+1)); echo "$out" | head -5; }
done
echo "traces: $pass agree, $fail disagree or failed"
[ $fail -eq 0 ]
