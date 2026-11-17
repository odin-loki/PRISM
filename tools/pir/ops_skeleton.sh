#!/usr/bin/env bash
# ops_skeleton.sh SKELETON.tsv[.gz] [MODULE_REGEX]
#
# Module skeleton for the device-model work: the entry points of each driver module, taken from its
# registration tables (struct pci_driver, file_operations, dev_pm_ops, ...). Input is the clang pass's
# ops skeleton (OS-Proving data/clang/passes/ops-skeleton-{linux,freebsd}.tsv.gz: module, table,
# table_type, file, line, field, role, function). The role is refined here from the table type and
# the designated field name (the clang pass guesses it from the name alone). Output, TSV with header:
#   module  role  table_type  table  field  function  file  line
# Only the eight entry-point roles are emitted: probe remove open close irq suspend resume ioctl.
# Read-only: no model, no judgement; a role that cannot be decided is dropped and counted on stderr.
set -euo pipefail
IN="${1:?usage: ops_skeleton.sh SKELETON.tsv[.gz] [MODULE_REGEX]}"
RX="${2:-.}"
cat_in() { case "$IN" in *.gz) gzip -dc "$IN";; *) cat "$IN";; esac; }
cat_in | awk -F'\t' -v rx="$RX" '
BEGIN { OFS = "\t"; print "module", "role", "table_type", "table", "field", "function", "file", "line" }
NR == 1 && $1 == "module" { next }
{
    mod = $1; tbl = $2; ttype = $3; file = $4; line = $5; field = $6; orole = $7; fn = $8
    if (mod !~ rx || fn == "") next
    role = ""
    if (ttype ~ /(platform|pci|i2c|spi|usb|amba|mdio|pcmcia|virtio|hid|rapidio)_driver$/) {
        if (field == "probe" || field == "probe_new") role = "probe"
        else if (field == "remove" || field == "remove_new" || field == "disconnect") role = "remove"
    } else if (ttype ~ /file_operations$/) {
        if (field == "open") role = "open"
        else if (field == "release") role = "close"
        else if (field ~ /ioctl$/) role = "ioctl"
    } else if (ttype ~ /dev_pm_ops$/) {
        if (field ~ /^(suspend|freeze|poweroff)(_late|_noirq)?$/) role = "suspend"
        else if (field ~ /^(resume|thaw|restore)(_early|_noirq)?$/) role = "resume"
    } else if (ttype ~ /net_device_ops$/) {
        if (field == "ndo_open") role = "open"
        else if (field == "ndo_stop") role = "close"
    } else if (ttype ~ /(kobj_method_t|device_method_t)/ || ttype ~ /^kobj_method_t/) {
        # FreeBSD DEVMETHOD / KOBJMETHOD entries: the name is in field when designated, else in orole
        m = (field != "" ? field : orole)
        if (m == "probe") role = "probe"
        else if (m == "attach") role = "probe"
        else if (m == "detach") role = "remove"
        else if (m == "suspend") role = "suspend"
        else if (m == "resume") role = "resume"
        else if (m == "open") role = "open"
        else if (m == "close") role = "close"
        else if (m == "ioctl") role = "ioctl"
    } else if (ttype ~ /(^|struct |const struct )(irq|interrupt)_/ || field ~ /(^|_)(irq|interrupt|isr)_?handler$/) {
        role = "irq"
    }
    if (role == "") { dropped++; next }
    print mod, role, ttype, tbl, field, fn, file, line
    kept[role]++
}
END {
    for (r in kept) printf "ops_skeleton: %s %d\n", r, kept[r] > "/dev/stderr"
    printf "ops_skeleton: dropped (role not decided) %d\n", dropped + 0 > "/dev/stderr"
}'
