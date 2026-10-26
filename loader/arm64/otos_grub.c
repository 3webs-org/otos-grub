/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2024  otos-grub contributors
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

/* otos-grub: the real GRUB module, replacing otos-coreinit's own
 * tools/grub-xnu-arm64 (a port of GRUB's xnu loader, which exists to
 * boot real macOS, not this project -- see ../../README.md). Unlike
 * otos-boot, this module forwards the raw firmware FDT without parsing
 * it. Coreinit remains the sole owner of hardware discovery.
 *
 * Command: otos_load <saski-macho> <coreinit-macho>. Prepares
 * everything (reads both files, builds the boot-protocol
 * objects, places every image via the relocator) and defers the
 * actual jump to GRUB's own `boot` command, via grub_loader_set --
 * the same convention every other GRUB loader uses (see
 * grub-core/loader/arm64/xen_boot.c, this module's own template for
 * the registration/file-I/O shape grub-xnu-arm64 didn't demonstrate).
 */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/command.h>
#include <grub/file.h>
#include <grub/loader.h>
#include <grub/machoload.h>
#include <grub/macho.h>
#include <grub/efi/efi.h>
#include <grub/cpu/relocator.h>
#include <grub/i18n.h>

#include "../../include/boot_protocol.h"
#include "../../include/capnp_list.h"

GRUB_MOD_LICENSE ("GPLv3+");

/* GCC lowers the by-value grub_relocator64_state argument to a memcpy even
 * with builtins disabled. GRUB exports grub_memcpy, not libc memcpy, so keep
 * the compiler-generated call module-local instead of leaving an unresolved
 * symbol that makes insmod fail silently. */
void *
memcpy (void *dest, const void *src, grub_size_t len)
{
  return grub_memcpy (dest, src, len);
}

/* Position-independent allocator code linked into this module only so its
 * bytes can be copied into the persistent relocator region below. */
extern grub_uint8_t otos_bump_alloc_start[];
extern grub_uint8_t otos_bump_finalize[];
extern grub_uint8_t otos_bump_alloc_end[];

/* Mach-O constants GRUB's own grub/macho.h doesn't define, because its
 * only consumer (grub-core/loader/xnu.c) never needed them: XNU is
 * entered via LC_UNIXTHREAD (GRUB_MACHO_CMD_THREAD, already in
 * macho.h), never LC_MAIN. This project's own build (ld64.lld, no
 * special flags) emits LC_MAIN for an ordinary PIE Mach-O -- see
 * otos-coreinit/include/boot_protocol.h's own top comment, and
 * otos-boot/tools/macho_flatten.py, which resolves the identical
 * value in Python for the compile-time-fixed target. LC_SYMTAB is
 * needed for exactly one lookup this module makes that otos-boot's
 * build-time `nm` call didn't need to: saski's own boot_protocol_start
 * symbol, resolved at RUNTIME against whatever saski binary the user
 * actually points this module at (see find_symbol64 below). */
#define LC_MAIN 0x80000028u
#define LC_SYMTAB 0x2u

struct lc_main_cmd
{
  grub_uint32_t cmd;
  grub_uint32_t cmdsize;
  grub_uint64_t entryoff;
  grub_uint64_t stacksize;
};

struct lc_symtab_cmd
{
  grub_uint32_t cmd;
  grub_uint32_t cmdsize;
  grub_uint32_t symoff;
  grub_uint32_t nsyms;
  grub_uint32_t stroff;
  grub_uint32_t strsize;
};

/* struct nlist_64, Mach-O's own 64-bit symbol-table entry -- not in
 * GRUB's grub/macho.h either, same reason as LC_MAIN/LC_SYMTAB above:
 * nothing in this codebase has ever needed to read a Mach-O symbol
 * table before. */
struct macho_nlist_64
{
  grub_uint32_t n_strx;
  grub_uint8_t n_type;
  grub_uint8_t n_sect;
  grub_uint16_t n_desc;
  grub_uint64_t n_value;
};

/* Walks a loaded grub_macho_t's 64-bit load commands looking for
 * LC_MAIN, the same way grub-core/loader/arm64/xnu.c's own
 * grub_xnu_arm64_get_entry_point64 walks the same array looking for
 * LC_UNIXTHREAD (GRUB_MACHO_CMD_THREAD) -- identical technique,
 * different target command. Returns 0 if none found (the caller
 * treats that as a load failure: every Mach-O this protocol is
 * willing to boot has one, by construction of this project's own
 * build). */
static grub_uint64_t
find_entry_lc_main (grub_macho_t macho)
{
  grub_uint8_t *p = macho->cmds64;
  grub_uint8_t *end = p + macho->cmdsize64;

  while (p + 8 <= end)
    {
      grub_uint32_t cmd = ((grub_uint32_t *) p)[0];
      grub_uint32_t size = ((grub_uint32_t *) p)[1];
      if (size < 8 || p + size > end)
	break;
      if (cmd == LC_MAIN && size >= sizeof (struct lc_main_cmd))
	{
	  struct lc_main_cmd *lc = (struct lc_main_cmd *) p;
	  /* entryoff is a FILE offset, resolved against whichever
	   * segment's own fileoff range contains it -- same
	   * resolution otos-boot/tools/macho_flatten.py does in
	   * Python, written here directly against cmds64 since
	   * that's what we already have. */
	  grub_uint8_t *q = macho->cmds64;
	  while (q + 8 <= end)
	    {
	      grub_uint32_t qcmd = ((grub_uint32_t *) q)[0];
	      grub_uint32_t qsize = ((grub_uint32_t *) q)[1];
	      if (qsize < 8 || q + qsize > end)
		break;
	      if (qcmd == GRUB_MACHO_CMD_SEGMENT64)
		{
		  struct grub_macho_segment64 *seg = (struct grub_macho_segment64 *) q;
		  if (seg->fileoff <= lc->entryoff
		      && lc->entryoff < seg->fileoff + seg->filesize)
		    return seg->vmaddr + (lc->entryoff - seg->fileoff);
		}
	      q += qsize;
	    }
	  return 0;
	}
      p += size;
    }
  return 0;
}

/* Reads LC_SYMTAB and scans it for a defined symbol by name, returning
 * its value (an address) or 0 if not found. This is the one piece of
 * real Mach-O symbol-table parsing this project has needed anywhere --
 * otos-boot's own build-time tooling resolves the equivalent lookup
 * (boot_protocol_start's address) with `nm` against one known saski
 * build instead, which otos-grub cannot do: it loads whatever saski
 * binary the user's grub.cfg points it at, so the lookup has to be
 * real and done at module-load time, not baked in advance. */
static grub_uint64_t
find_symbol64 (grub_macho_t macho, const char *filename, const char *name)
{
  grub_uint8_t *p = macho->cmds64;
  grub_uint8_t *end = p + macho->cmdsize64;
  struct lc_symtab_cmd symtab = { 0, 0, 0, 0, 0, 0 };
  int found = 0;

  while (p + 8 <= end)
    {
      grub_uint32_t cmd = ((grub_uint32_t *) p)[0];
      grub_uint32_t size = ((grub_uint32_t *) p)[1];
      if (size < 8 || p + size > end)
	break;
      if (cmd == LC_SYMTAB && size >= sizeof (struct lc_symtab_cmd))
	{
	  grub_memcpy (&symtab, p, sizeof (symtab));
	  found = 1;
	  break;
	}
      p += size;
    }
  if (!found || symtab.nsyms == 0)
    return 0;

  struct macho_nlist_64 *syms = grub_malloc ((grub_size_t) symtab.nsyms
					     * sizeof (*syms));
  char *strtab = grub_malloc (symtab.strsize);
  grub_uint64_t result = 0;
  if (!syms || !strtab)
    goto out;

  if (grub_file_seek (macho->file, macho->offset64 + symtab.symoff)
      == (grub_off_t) -1
      || grub_file_read (macho->file, syms,
			 (grub_size_t) symtab.nsyms * sizeof (*syms))
      != (grub_ssize_t) ((grub_size_t) symtab.nsyms * sizeof (*syms)))
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"), filename);
      goto out;
    }
  if (grub_file_seek (macho->file, macho->offset64 + symtab.stroff)
      == (grub_off_t) -1
      || grub_file_read (macho->file, strtab, symtab.strsize)
      != (grub_ssize_t) symtab.strsize)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"), filename);
      goto out;
    }

  {
    grub_uint32_t i;
    for (i = 0; i < symtab.nsyms; i++)
      {
	if (syms[i].n_strx == 0 || syms[i].n_strx >= symtab.strsize)
	  continue;
	if (grub_strcmp (strtab + syms[i].n_strx, name) == 0)
	  {
	    result = syms[i].n_value;
	    break;
	  }
      }
  }

out:
  grub_free (syms);
  grub_free (strtab);
  return result;
}

/* GRUB's kernel exports the raw firmware FDT lookup. This module does
 * not parse the blob; it forwards the physical pointer in x0 so
 * coreinit remains the sole owner of hardware-description parsing. */
extern void *grub_efi_get_firmware_fdt (void);

/* ---- Placing an opened Mach-O via the relocator: allocate exactly
 * the span grub_macho_size64 reports, at the Mach-O's own declared
 * vmaddr (no sliding -- same convention otos-boot/tools/
 * macho_flatten.py and saski's own build already use throughout this
 * protocol), then let grub_macho_load64 read every segment straight
 * from the file into place (offset=(char*)0, so `offset + vmaddr`
 * lands exactly on the allocated chunk -- see grub-core/loader/
 * machoXX.c's own do_load for why offset means that). flags=0, not
 * GRUB_MACHO_NOBSS: BSS must be zeroed, the same thing otos-boot's
 * own flattener does by starting from a zeroed buffer.
 *
 * Deliberately does NOT exclude __LINKEDIT by name the way otos-boot/
 * tools/macho_flatten.py now does after finding that bug -- GRUB's
 * own grub_macho_size64/load64 don't either (neither excludes any
 * segment by name, only by filesize/vmsize), and here that's harmless
 * rather than wrong: both the allocation size and the copy come from
 * the SAME GRUB call, so there is no second, independently-computed
 * length for them to disagree with the way the Python version's own
 * length mismatch was a real bug. The cost is a few KiB of wasted,
 * unused memory for __LINKEDIT's own symbol/string tables, not a
 * correctness problem -- not worth fighting GRUB's own API shape for,
 * per this module's own "follow the same shape" mandate. */
static grub_err_t
place_macho (struct grub_relocator *rel, const char *filename,
	    grub_uint64_t *out_base, grub_uint64_t *out_entry_main)
{
  grub_macho_t macho = grub_macho_open (filename, GRUB_FILE_TYPE_NONE, 1);
  grub_uint64_t start, end;
  grub_relocator_chunk_t chunk;
  grub_err_t err;

  if (!macho)
    return grub_errno;

  err = grub_macho_size64 (macho, &start, &end, 0, filename);
  if (err != GRUB_ERR_NONE)
    {
      grub_macho_close (macho);
      return err;
    }

  err = grub_relocator_alloc_chunk_addr (rel, &chunk, start, end - start);
  if (err != GRUB_ERR_NONE)
    {
      grub_macho_close (macho);
      return err;
    }

  err = grub_macho_load64 (macho, filename, (char *) 0, 0, NULL);
  if (err != GRUB_ERR_NONE)
    {
      grub_macho_close (macho);
      return err;
    }

  if (out_entry_main)
    *out_entry_main = find_entry_lc_main (macho);
  *out_base = start;

  grub_macho_close (macho);
  return GRUB_ERR_NONE;
}

/* ---- Deferred state between otos_load and the actual boot (grub_loader_set
 * calls otos_boot later, from GRUB's own `boot` command -- see this
 * module's own top comment). ---- */
static struct grub_relocator *otos_relocator;
static grub_uint64_t otos_fdt_phys;
static grub_uint64_t otos_id_addr;
static grub_uint64_t otos_entry_stack_top;
static grub_uint64_t otos_boot_protocol_start;
static grub_uint64_t otos_init_entry;
static int otos_loaded;

static grub_err_t
otos_unload (void)
{
  if (otos_relocator)
    grub_relocator_unload (otos_relocator);
  otos_relocator = NULL;
  otos_loaded = 0;
  return GRUB_ERR_NONE;
}

static grub_err_t
otos_boot (void)
{
  struct grub_relocator64_state state;

  if (!otos_loaded)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "otos_load was not run");

  grub_memset (&state, 0, sizeof (state));
  state.x0 = otos_fdt_phys;
  state.x1 = otos_init_entry; /* coreinit's real LC_MAIN entry, found by
			       * place_macho in grub_cmd_otos_load */
  state.x2 = otos_id_addr;
  state.pc = otos_boot_protocol_start;
  state.sp = otos_entry_stack_top;

  return grub_relocator64_boot (otos_relocator, state, 0);
}

static grub_command_t cmd_otos_load;

static grub_err_t
grub_cmd_otos_load (grub_command_t cmd __attribute__ ((unused)),
		    int argc, char *argv[])
{
  grub_uint64_t saski_base;
  grub_uint64_t coreinit_base, coreinit_entry;
  grub_relocator_chunk_t allocator_chunk;
  grub_uint8_t *id;
  size_t id_len;
  grub_err_t err;

  if (argc != 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "usage: otos_load <saski-macho> <coreinit-macho>");

  grub_loader_unset ();
  otos_unload ();

  otos_relocator = grub_relocator_new ();
  if (!otos_relocator)
    return grub_errno;

  void *firmware_fdt = grub_efi_get_firmware_fdt ();
  if (!firmware_fdt)
    {
      err = grub_error (GRUB_ERR_BAD_OS,
			"no raw FDT in the EFI configuration table");
      goto fail;
    }
  otos_fdt_phys = (grub_uint64_t) (grub_addr_t) firmware_fdt;

  err = place_macho (otos_relocator, argv[0], &saski_base, NULL);
  if (err != GRUB_ERR_NONE)
    goto fail;
  err = place_macho (otos_relocator, argv[1], &coreinit_base, &coreinit_entry);
  if (err != GRUB_ERR_NONE)
    goto fail;
  if (coreinit_entry == 0)
    {
      err = grub_error (GRUB_ERR_BAD_OS,
			"%s: no LC_MAIN -- see otos-coreinit/include/"
			"boot_protocol.h's own note on the entry-point "
			"convention this protocol requires", argv[1]);
      goto fail;
    }

  /* Unlike coreinit's entry (LC_MAIN, found by place_macho above),
   * saski's own boot_protocol_start is not the Mach-O's declared
   * entry point at all (that is still _start, for every OTHER boot
   * path this same binary also supports) -- it has to be resolved by
   * name, from the real symbol table, since this module loads
   * whatever saski binary the user's grub.cfg points it at and cannot
   * know its layout in advance. See find_symbol64's own comment. */
  {
    grub_macho_t saski_macho = grub_macho_open (argv[0], GRUB_FILE_TYPE_NONE, 1);
    if (!saski_macho)
      {
	err = grub_errno;
	goto fail;
      }
    otos_boot_protocol_start = find_symbol64 (saski_macho, argv[0],
					      "_boot_protocol_start");
    grub_macho_close (saski_macho);
    if (otos_boot_protocol_start == 0)
      {
	err = grub_error (GRUB_ERR_BAD_OS,
			  "%s: no _boot_protocol_start symbol -- this "
			  "saski build does not implement the new boot "
			  "protocol (see arch/boot_aarch64.S there)",
			  argv[0]);
	goto fail;
      }
  }
  otos_init_entry = coreinit_entry;

  /* InitDescription, coreinit allocator code and caller-owned
   * entry stack occupy one persistent relocator region. They go
   * anywhere free -- unlike saski/coreinit themselves, nothing has a
   * baked-in expectation of their address (coreinit reaches them
   * through pointers passed in registers/init_boot_info, never a
   * fixed constant), so this module uses the RELOCATOR'S FLEXIBLE
   * allocator (grub_relocator_alloc_chunk_align) instead of hardcoding
   * the same fixed QEMU-virt-only addresses otos-boot's own compile-
   * time build uses -- this is the more general, genuinely-runtime
   * choice the GRUB module can afford to make that otos-boot's own
   * fixed target cannot. */
  {
    id = grub_malloc (4096);
    if (!id)
      {
	err = grub_errno;
	goto fail;
      }

    struct init_variant variants[1] = {
      { .arch_tag = BOOT_PROTO_ARCH_AARCH64,
	.kernel_macho_phys = saski_base, .kernel_macho_len = 0,
	.init_macho_phys = coreinit_base, .init_macho_len = 0 },
    };
    struct init_description_data id_data = {
      .magic = INIT_DESCRIPTION_MAGIC,
      .coreinit_allocator_phys = 0,
    };
    id_len = capnp_build_list_root (id, 4096,
                                    (const grub_uint64_t *) &id_data,
				    BOOT_PROTO_ID_DATA_WORDS,
				    (const grub_uint64_t *) variants, 1,
				    BOOT_PROTO_INIT_VARIANT_WORDS);

    if (id_len == 0)
      {
	err = grub_error (GRUB_ERR_OUT_OF_MEMORY,
			  "boot-protocol object encoding overflowed its "
			  "scratch buffer");
	goto fail;
      }

    err = grub_relocator_alloc_chunk_align (otos_relocator, &allocator_chunk,
					    0, (grub_phys_addr_t) -1,
					    0x110000, 4096, 0, 0);
    if (err != GRUB_ERR_NONE)
      goto fail;
    otos_id_addr = get_physical_target_address (allocator_chunk);
    grub_uint8_t *region = get_virtual_current_address (allocator_chunk);

    /* Flat Cap'n Proto object: header, root pointer, then the seven
     * boot_allocator data words. Code and the 64 KiB entry stack follow;
     * `used` covers all of them so later allocations cannot overlap the
     * stack that remains active across the saski-to-coreinit handoff. */
    {
      grub_size_t allocator_off = (id_len + 7) & ~(grub_size_t) 7;
      grub_size_t code_off = (allocator_off + 72 + 15) & ~(grub_size_t) 15;
      grub_size_t code_len = (grub_size_t) (otos_bump_alloc_end -
					     otos_bump_alloc_start);
      grub_size_t stack_off = (code_off + code_len + 15) & ~(grub_size_t) 15;
      grub_size_t used = stack_off + 0x10000;
      if (used > 0x110000)
	{
	  err = grub_error (GRUB_ERR_OUT_OF_MEMORY,
			    "boot allocator metadata exceeds its region");
	  goto fail;
	}

      grub_uint8_t *msg = region + allocator_off;
      grub_memset (msg, 0, 72);
      ((grub_uint32_t *) msg)[1] = 8; /* one root word + seven data words */
      ((grub_uint64_t *) (msg + 8))[0] = 7ULL << 32;
      struct boot_allocator *allocator = (struct boot_allocator *) (msg + 16);
      grub_uint64_t code_phys = otos_id_addr + code_off;
      allocator->magic = BOOT_ALLOCATOR_MAGIC;
      allocator->alloc = code_phys;
      allocator->finalize = code_phys +
	(grub_uint64_t) (otos_bump_finalize - otos_bump_alloc_start);
      allocator->region_base = otos_id_addr;
      allocator->region_len = 0x110000;
      allocator->used = used;
      allocator->finalized = 0;
      grub_memcpy (region + code_off, otos_bump_alloc_start, code_len);

      id_data.coreinit_allocator_phys = otos_id_addr + allocator_off;
      grub_size_t final_id_len = capnp_build_list_root (
        id, 4096, (const grub_uint64_t *) &id_data,
        BOOT_PROTO_ID_DATA_WORDS, (const grub_uint64_t *) variants, 1,
        BOOT_PROTO_INIT_VARIANT_WORDS);
      if (final_id_len != id_len || allocator_off < final_id_len)
	{
	  err = grub_error (GRUB_ERR_BAD_OS,
			    "InitDescription size changed or overlaps allocator");
	  goto fail;
	}
      grub_memcpy (region, id, final_id_len);
      otos_entry_stack_top = otos_id_addr + used;
    }

    grub_free (id);
  }

  otos_loaded = 1;
  grub_loader_set (otos_boot, otos_unload, 0);
  return GRUB_ERR_NONE;

fail:
  otos_unload ();
  return err;
}

GRUB_MOD_INIT (otos_grub)
{
  cmd_otos_load = grub_register_command ("otos_load", grub_cmd_otos_load, 0,
					 N_("Load a saski kernel and coreinit "
					    "init image via the otos boot "
					    "protocol."));
}

GRUB_MOD_FINI (otos_grub)
{
  grub_unregister_command (cmd_otos_load);
}
