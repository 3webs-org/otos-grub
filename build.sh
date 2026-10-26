#!/bin/sh
# Build otos-grub as a loadable .mod against a stock distro GRUB,
# following otos-coreinit/tools/grub-xnu-arm64/build.sh's own shape
# (this repo's own README says to follow it, not reinvent it) --
# same relocatable-link-then-objcopy packaging, same relocation-type
# verification step, same CC-must-be-gcc constraint. What's different:
# no prepare-src.sh step (this module patches nothing in GRUB_SRC --
# it does not reuse any upstream loader/*.c the way grub-xnu-arm64
# reuses grub-core/loader/xnu.c), and the file list is this project's
# own (loader/arm64/otos_grub.c and the extended
# relocator from lib/arm64/).
#
# CC MUST BE AARCH64 GCC, NOT CLANG -- see grub-xnu-arm64/build.sh's
# own comment for why (clang fuses adrp+add into relocation types
# GRUB's own arm64 relocator, kern/arm64/dl.c, does not implement;
# the failure is silent at insmod, not a build error).
#
# Uses: GRUB_SRC (configured source tree), AARCH64_GCC, LD_LLD (or a
# plain ld with -r support), OBJCOPY, READELF.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
GRUB_SRC=${GRUB_SRC:?set GRUB_SRC to a GRUB source tree}
AARCH64_GCC=${AARCH64_GCC:?set AARCH64_GCC to an aarch64 gcc (not clang)}
LD=${LD:-ld}
OBJCOPY=${OBJCOPY:-objcopy}
OUT=${OUT:-$HERE/build}

mkdir -p "$OUT/shim/grub/cpu"
# otos-grub's own arm64 headers (extended from grub-xnu-arm64's own --
# see include/grub/arm64/relocator.h's own top comment on the one
# addition, x4) layered the same way grub-xnu-arm64/build.sh already
# does: the configured tree's own arm64 headers first, then this
# repo's on top.
cp -r "$GRUB_SRC/include/grub/arm64/." "$OUT/shim/grub/cpu/" 2>/dev/null || true
cp "$HERE"/include/grub/arm64/*.h "$OUT/shim/grub/cpu/"

# -fno-builtin alone stops gcc ASSUMING memcpy/memset's semantics, but
# not from SYNTHESIZING calls to them in place of a struct copy or an
# init loop it recognizes -- a bare `memcpy`/`memset` call is an
# undefined symbol at insmod time, since GRUB's own kernel exports
# grub_memcpy/grub_memset, never the plain libc names.
# -fno-tree-loop-distribute-patterns turns that idiom recognition off.
CFLAGS="-ffreestanding -nostdlib -Os -fno-builtin -fno-stack-protector
        -fno-tree-loop-distribute-patterns
        -mno-outline-atomics -fshort-wchar
        -DGRUB_MACHINE_EFI=1 -D__aarch64__=1
        -I$OUT/shim -I$HERE -I$HERE/include -I$GRUB_SRC/include -I$GRUB_SRC -I$OUT"

cc1() { # <source> <grub-file-name> <output>
  # shellcheck disable=SC2086
  $AARCH64_GCC $CFLAGS -DGRUB_FILE="\"$2\"" -c "$1" -o "$OUT/$3.o"
}

$AARCH64_GCC $CFLAGS -DGRUB_FILE="\"loader/arm64/otos_grub.c\"" \
  -c "$HERE/loader/arm64/otos_grub.c" -o "$OUT/main.o"
cc1 "$HERE/lib/arm64/relocator.c"          lib/arm64/relocator.c       relocator
cc1 "$HERE/lib/arm64/relocator_boot.S"     lib/arm64/relocator_boot.S  boot
cc1 "$HERE/include/boot_allocator_aarch64.S" include/boot_allocator_aarch64.S allocator

$LD -r -o "$OUT/otos_grub.linked.o" \
  "$OUT/main.o" "$OUT/relocator.o" "$OUT/boot.o" "$OUT/allocator.o"

# .moddeps lists the stock modules providing our undefined symbols
# (grub_macho_* -- macho.mod; grub_file_open/grub_loader_set and
# friends are core, not a module, same as grub_efi_get_firmware_fdt --
# see loader/arm64/otos_grub.c's own comment on that one), so
# `insmod otos_grub` pulls them the way a distro module would.
printf 'otos_grub\0' > "$OUT/modname.bin"
printf 'macho\0' > "$OUT/moddeps.bin"
$OBJCOPY --add-section .modname="$OUT/modname.bin" \
         --add-section .moddeps="$OUT/moddeps.bin" \
         "$OUT/otos_grub.linked.o" "$OUT/otos_grub.mod"

# Fail the build, not the boot, if the relocator cannot handle what we
# emitted -- see grub-xnu-arm64/build.sh's own comment on why this
# check exists at all (a silent grub_dl_load failure looks identical
# to a module with no commands at the insmod prompt). Same nine types
# grub-core/kern/arm64/dl.c implements, verbatim from its switch.
SUPPORTED="R_AARCH64_ABS64
R_AARCH64_ADD_ABS_LO12_NC
R_AARCH64_LDST64_ABS_LO12_NC
R_AARCH64_CALL26
R_AARCH64_JUMP26
R_AARCH64_PREL32
R_AARCH64_ADR_GOT_PAGE
R_AARCH64_LD64_GOT_LO12_NC
R_AARCH64_ADR_PREL_PG_HI21"

printf '%s\n' "$SUPPORTED" | sort -u > "$OUT/supported-relocs.txt"
READELF=${READELF:-$(dirname "$OBJCOPY")/readelf}
"$READELF" -rW "$OUT/otos_grub.mod" > "$OUT/relocs.txt"
awk '{print $3}' "$OUT/relocs.txt" | grep '^R_' | sort -u > "$OUT/mine-relocs.txt"
if [ ! -s "$OUT/mine-relocs.txt" ]; then
  echo "no relocations read from $OUT/otos_grub.mod -- is $READELF working?" >&2
  exit 1
fi
BAD=$(comm -23 "$OUT/mine-relocs.txt" "$OUT/supported-relocs.txt")
if [ -n "$BAD" ]; then
  echo "unsupported relocation types (kern/arm64/dl.c cannot apply these):" >&2
  echo "$BAD" >&2
  exit 1
fi

echo "built $OUT/otos_grub.mod ($(wc -c < "$OUT/otos_grub.mod") bytes)"
