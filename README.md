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

## The protocol

Defined in full in [`include/boot_protocol.h`](include/boot_protocol.h),
vendored here from [`otos-coreinit`](https://github.com/3webs-org/otos-coreinit),
which owns the canonical copy. That header is the actual contract --
this section is a summary, not a second source of truth.

At kernel (saski) entry, three values, both architectures:

- **aarch64**: `x0` = a real, standard FDT physical address (kept so the
  resulting image still looks like a normal arm64 boot target). `x1` =
  init's already-resolved entry point. `x2` = InitDescription object's
  physical address.
- **x86_64**: `rdi` = raw ACPI RSDP, `rsi` = init entry point, `rdx` =
  InitDescription pointer.

**InitDescription** is a real Cap'n Proto object containing the variant
list and coreinit allocator pointer. The module forwards the raw firmware
FDT/ACPI pointer without parsing it; coreinit owns that parsing.

**Mach-O entry point**: the standard `LC_MAIN` load command. No
`ARM_THREAD_STATE64`/`x86_THREAD_STATE64` games needed -- `LC_MAIN` is
what any Mach-O linker (including the one this project's own build
already uses, ld64.lld) emits by default; faking a thread state via
`LC_UNIXTHREAD` was only ever necessary to satisfy GRUB's xnu loader's
own narrow parser, which this module replaces.

## Status: design stage, not yet implemented

This repository is currently scaffolding only -- the protocol above is
now a resolved, documented contract (`include/boot_protocol.h`), but
nothing in this repo actually parses a DT/ACPI table or builds a real
object from one yet. That implementation is the next step, not done
here.

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
