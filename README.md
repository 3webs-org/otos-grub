# otos-grub

A custom GRUB module for booting [otos-coreinit](https://github.com/3webs-org/otos-coreinit)'s
Mach-O-wrapped kernel (`saski`) and init (`coreinit`) images -- replacing
[`otos-coreinit/tools/grub-xnu-arm64`](https://github.com/3webs-org/otos-coreinit/tree/main/tools/grub-xnu-arm64),
which is a port of GRUB's own `xnu` loader (built for booting real macOS,
not this project).

## Why this exists, instead of extending the xnu loader

The xnu loader's boot handoff is shaped by XNU's own boot protocol: a
proprietary, 4 KiB `xnu_boot_args_v2` struct, a proprietary (non-FDT)
device-tree format for the rare cases a kext/ramdisk needs to ride along,
and Mach-O entry-point extraction that assumes an `x86_THREAD_STATE64`
(or, in the arm64 port, a hand-rolled `ARM_THREAD_STATE64` equivalent)
`LC_UNIXTHREAD`. None of that is a protocol this project designed --
reusing it meant `saski`'s kernel carried a DT/ACPI-parsing EFI-config-table
walk and a `macho_load` call it never actually needed to own, just to
unwrap what the xnu loader handed it.

This module speaks a protocol designed for this project instead: it does
the parsing itself (real DT on arm, ACPI on x86 -- both just "a device
this loader knows how to read," feeding one shared schema) and hands the
kernel an already-resolved entry point plus a couple of opaque pointers,
so the kernel's own job shrinks to almost nothing.

## The protocol (aarch64, current design)

At kernel entry:
- **x0** -- a real, standard FDT physical address. Kept so the resulting
  image still looks like a normal arm64 boot target to anything that only
  speaks the standard protocol; the kernel itself never reads it.
- **x1** -- the physical address of a Cap'n Proto-encoded
  machine-description object this module built from whatever firmware
  actually handed it (DT or ACPI) -- opaque to the kernel, forwarded to
  `coreinit` to decode.
- **x2** -- the physical address of a Cap'n Proto-encoded
  init-description object: a list of per-architecture init-binary
  variants, for `coreinit`'s own SMP bring-up to consult later if a
  secondary core turns out to be a different architecture than the BSP.
  Opaque to the kernel; just has to survive the handoff.
- **x3** -- the init program's already-resolved entry point. This module
  parses the Mach-O kernel and init images itself (segments and entry
  point) -- the one piece the xnu loader fakes via `ARM_THREAD_STATE64`/
  `x86_THREAD_STATE64` games, done directly here instead.

x86_64's register mapping is the same idea (SysV integer-argument
registers, by analogy) but not yet pinned down -- this project started
aarch64-first.

## Status: design stage, not yet implemented

This repository is currently scaffolding only. Real, load-bearing design
decisions that are **not yet made** -- don't assume any of these from
reading this README alone:

- The exact field list of both Cap'n Proto schemas (machine-description,
  init-description/variant-list).
- Which repository hosts those schemas (consumed by this module, by
  `otos-boot`, and by `otos-coreinit`'s kernel/init -- three repos that
  all need to agree on one wire format).
- The exact Mach-O entry-point convention this module uses (replacing
  the `ARM_THREAD_STATE64`/`LC_UNIXTHREAD` reuse from GRUB's xnu loader).
- x86_64's register mapping for the same protocol.

## Prior art worth reusing as a template

`otos-coreinit/tools/grub-xnu-arm64` already worked out the real mechanics
of building an out-of-tree GRUB module against a stock GRUB source tree:
a `prepare-src.sh` that makes narrowly-scoped, idempotent patches to the
one or two upstream lines that hardcode something architecture-specific;
vendored `include/grub/<arch>/*.h` headers for whatever `grub/cpu/*.h`
upstream doesn't ship for the target; a `build.sh` that layers those
vendored headers over a real `GRUB_SRC` checkout via an include shim,
compiles with `-DGRUB_FILE`, does a relocatable `ld -r` link, and packages
the result as a real `.mod` via `objcopy --add-section .modname/.moddeps`;
and, crucially, a relocation-type verification step (`readelf` against
the exact switch statement `grub-core/kern/<arch>/dl.c` implements) that
turns GRUB's silent `insmod` failure mode into a build-time one. This
module should follow the same shape, not reinvent it.

GRUB's generic Mach-O reader (`grub-core/loader/machoXX.c`, `macho.mod`)
is architecture-parameterized but not XNU-specific in its core
load-command parsing -- only entry-point extraction assumes an Apple/x86
convention, and that's exactly the piece this module replaces with its
own, real-DT-based convention.

No real GRUB source tree is vendored in this repo (same as the xnu-loader
port -- `GRUB_SRC` must point at one, fetched separately).
