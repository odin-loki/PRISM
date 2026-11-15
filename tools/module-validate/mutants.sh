#!/bin/bash
# mutants.sh PRISM_MODULE_BIN WORKDIR : the full PL031 cross-check of docs/DEVICE_MODEL.md section 9.5.
# Shipped model on 40 broad traces and 30 alarm traces, then seven mutants of the model on 20 + 20 traces each:
# a mutant the traces cannot tell from the shipped model is reported as surviving.
set -u
PM=$1; W=$2
HERE=$(cd "$(dirname "$0")" && pwd)
PDM=$HERE/../../src/prism/pir/models/device/pl031.pdm
mkdir -p "$W"
trial() { # label pdm seed0 n nops
  out=$(SEED0=$3 bash "$HERE/run.sh" "$PM" "$2" "$W/w_$(echo "$1" | tr -c 'a-z0-9' _)" "$4" "$5" 2>&1)
  echo "$1: $(echo "$out" | grep -c '^seed [0-9]*: OK') agree, $(echo "$out" | grep -c '^seed [0-9]*: FAIL') disagree"
}
trial "shipped broad"  "$PDM" 1   40 24
trial "shipped alarm"  "$PDM" 100 30 0
mut() { name=$1; shift; sed "$@" "$PDM" > "$W/mut_$name.pdm"
  cmp -s "$PDM" "$W/mut_$name.pdm" && { echo "mutant $name: the sed changed nothing"; return; }
  trial "mutant $name broad" "$W/mut_$name.pdm" 1   20 24
  trial "mutant $name alarm" "$W/mut_$name.pdm" 100 20 0; }
mut icr_any    -e 's/RIS = RIS \& ~value;/RIS = 0;/'
mut mr_noimm   -e 's/if (((MR - (now + tick_offset)) \& 0xffffffff) == 0) { RIS = 1; armed = 0; } else { armed = 1; rem = MR - (now + tick_offset); }/armed = 1; rem = MR - (now + tick_offset);/'
mut cr_zero    -e 's/    read = 1;/    read = 0;/'
mut imsc_wide  -e 's/register IMSC @0x10 : u32 RW reset 0 mask 1;/register IMSC @0x10 : u32 RW reset 0 mask 0xffffffff;/'
mut late_fire  -e 's/if (rem <= dt) {/if (rem < dt) {/'
mut lr_nooff   -e 's/      tick_offset = value - now;/      tick_offset = tick_offset;/'
mut never_fire -e 's/havoc fire where fire <= 1;/havoc fire where fire <= 0;/'
