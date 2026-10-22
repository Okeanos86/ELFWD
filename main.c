/*
 * ELFWD - Generic PS2 ELF forwarder with argv spoofing, optional IOP
 *         reset/module loading and RAM patch support, driven by a
 *         plain-text key=value config file (ELFWD.CFG).
 * Copyright (c) 2026 Okeanos
 *
 * Licensed under the Academic Free License version 2.0
 * See LICENSE for the full license text.
 *
 * By default ELFWD loads no IOP modules and does not reset the IOP: it
 * relies on the module set already resident from whichever launcher
 * started it. reset= and irx= in the CFG opt in to something different.
 *
 * Any error returns from main() (back to OSDSYS) without showing anything.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>

#include "cfg.h"

#define BOOT_RETRY_COUNT 20      /* retries after the first attempt        */
#define BOOT_RETRY_US    250000  /* 20 * 250 ms = ~5 s                     */

// ---------------------------------------------------------------------
// Minimal ELF32 structures
// ---------------------------------------------------------------------
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define PT_LOAD 1

static char config_path[512];
static char launcher_dir[512];       // directory of the launcher, trailing '/'
static char irx_full[CFG_MAX_IRX][1024]; // irx= paths after resolution against the launcher folder
static char cfg_buf[CFG_MAX_FILE + 2];
static Cfg  cfg;
static uint32_t patch_addr[CFG_MAX_PATCHES];
static char *exec_argv[1 + CFG_MAX_ARGS + 1];

// IRX images read into RAM before an IOP reset (reset=1): the reset kills
// every device driver, so anything not in the IOP ROM must already be in
// EE memory when the reset happens. 64-byte aligned for DMA.
static uint8_t irx_pool[CFG_MAX_IRX_BYTES] __attribute__((aligned(64)));
static struct { void *ptr; uint32_t size; } irx_img[CFG_MAX_IRX];

// ---------------------------------------------------------------------
// Optional diagnostics (make DEBUG=1 -> -DELFWD_DEBUG). Errors normally
// leave silently; in this build the screen border is held for ~3 s in a
// colour that says which stage failed:
//   red = CFG path/read   yellow = CFG validation   green = irx=
//   blue = boot= load     magenta = patch=
// For irx=, the border first blinks white N times (N = failing irx= line).
// ---------------------------------------------------------------------
static int dbg_irx_index;

#ifdef ELFWD_DEBUG
#define GS_BGCOLOR (*(volatile uint64_t *)0x120000E0)
static void dbg_color(uint32_t bgr) { GS_BGCOLOR = bgr; }
static void dbg_hold(uint32_t bgr) {
    int i;
    for (i = 0; i < dbg_irx_index; i++) {
        dbg_color(0xFFFFFF); usleep(250000);
        dbg_color(0x000000); usleep(250000);
    }
    dbg_color(bgr);
    usleep(3000000);
}
#define BAIL(color) bail_dbg(color)
static int bail_dbg(uint32_t bgr);
#else
#define BAIL(color) bail()
#endif

// Returns to OSDSYS silently. Kept in one place so the "how do we leave"
// decision (plain return vs. launching the browser) is a one-line change.
static int bail(void) {
    SifExitRpc();
    return -1;
}

#ifdef ELFWD_DEBUG
static int bail_dbg(uint32_t bgr) { dbg_hold(bgr); return bail(); }
#endif

// Builds "<dir of argv[0]>ELFWD.CFG". argv[0] may carry a ";1" suffix.
static int build_config_path(int argc, char *argv[]) {
    char self[256];
    char *semi, *slash;
    size_t dir_len;

    if (argc <= 0 || !argv[0] || !argv[0][0]) return -1;

    strncpy(self, argv[0], sizeof(self) - 1);
    self[sizeof(self) - 1] = '\0';

    semi = strchr(self, ';');
    if (semi) *semi = '\0';

    slash = strrchr(self, '/');
    if (!slash) return -1;

    dir_len = (size_t)(slash - self + 1);
    if (dir_len + sizeof("ELFWD.CFG") > sizeof(config_path)) return -1;

    memcpy(launcher_dir, self, dir_len);
    launcher_dir[dir_len] = '\0';
    memcpy(config_path, self, dir_len);
    strcpy(config_path + dir_len, "ELFWD.CFG");
    return 0;
}

// Reads the whole file (up to CFG_MAX_FILE + 1 bytes so an oversized file
// is detected by cfg_parse). Returns the length, or -1 on I/O error.
static long read_config(const char *path) {
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) return -1;
    n = fread(cfg_buf, 1, CFG_MAX_FILE + 1, f);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    cfg_buf[n] = '\0';
    return (long)n;
}

// Reads the PT_LOAD program headers of the ELF at `path`.
static int elf_load_layout(const char *path, CfgLayout *layout) {
    FILE *ef = fopen(path, "rb");
    Elf32_Ehdr eh;
    int i;

    layout->count = 0;
    if (!ef) return -1;

    if (fread(&eh, sizeof(eh), 1, ef) != 1) { fclose(ef); return -1; }

    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F' ||
        eh.e_phentsize < sizeof(Elf32_Phdr)) {
        fclose(ef);
        return -1;
    }

    for (i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        if (fseek(ef, (long)(eh.e_phoff + (uint32_t)i * eh.e_phentsize), SEEK_SET) != 0 ||
            fread(&ph, sizeof(ph), 1, ef) != 1) {
            fclose(ef);
            return -1;
        }
        if (ph.p_type == PT_LOAD) {
            if (layout->count >= CFG_MAX_SEGS) { fclose(ef); return -1; }
            layout->seg[layout->count].offset = ph.p_offset;
            layout->seg[layout->count].vaddr  = ph.p_vaddr;
            layout->seg[layout->count].filesz = ph.p_filesz;
            layout->count++;
        }
    }

    fclose(ef);
    return 0;
}

// An irx= path with no device (no ':') means "relative to the launcher":
// the launcher's directory is prefixed. Done on the EE for both
// reset modes, because with reset=0 the IOP cannot resolve relative paths.
static int resolve_irx_paths(void) {
    int i;
    for (i = 0; i < cfg.nirx; i++) {
        if (cfg_resolve_irx_path(launcher_dir, cfg.irx[i].path,
                                 irx_full[i], sizeof(irx_full[i])) < 0) {
            dbg_irx_index = i + 1;
            return -1;
        }
        cfg.irx[i].path = irx_full[i];
    }
    return 0;
}

// With reset=1, reads every irx= that is not under rom0: into irx_pool,
// while the launcher's devices are still alive. Any unreadable, empty or
// oversized file is an error (nothing on the IOP has been touched yet).
static int read_irx_images(void) {
    size_t used = 0;
    int i;

    memset(irx_img, 0, sizeof(irx_img));

    for (i = 0; i < cfg.nirx; i++) {
        FILE *f;
        long size;
        size_t room = (used < CFG_MAX_IRX_BYTES) ? CFG_MAX_IRX_BYTES - used : 0;

        if (cfg_path_is_rom(cfg.irx[i].path)) continue;

        dbg_irx_index = i + 1;
        f = fopen(cfg.irx[i].path, "rb");
        if (!f) return -1;

        if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
            (size_t)size > room || fseek(f, 0, SEEK_SET) != 0 ||
            fread(irx_pool + used, 1, (size_t)size, f) != (size_t)size) {
            fclose(f);
            return -1;
        }
        fclose(f);

        irx_img[i].ptr  = irx_pool + used;
        irx_img[i].size = (uint32_t)size;
        used += ((size_t)size + 63) & ~(size_t)63;
    }

    dbg_irx_index = 0;
    FlushCache(0);   // images were written by the CPU; make sure RAM has them
    return 0;
}

// Same sequence as the previous ELFWD build that ran on hardware: RPC is
// still up (SifInitRpc in main) when SifIopReset is issued. Do NOT call
// SifExitRpc() first, SifIopReset needs the SIF channel alive.
static void reset_iop(void) {
    while (!SifIopReset("", 0)) { }
    while (!SifIopSync()) { }
    SifInitRpc(0);
}

// Starts the irx= modules in file order: from the RAM image when one was
// read (reset=1, non-rom0), otherwise by path.
static int load_irx_modules(void) {
    int i;
    for (i = 0; i < cfg.nirx; i++) {
        char args[CFG_IRX_ARGS_MAX];
        int arglen = cfg_irx_args(&cfg.irx[i], args, sizeof(args));
        int result = 0;
        int id;

        if (arglen < 0) { dbg_irx_index = i + 1; return -1; }

        if (irx_img[i].ptr) {
            id = SifExecModuleBuffer(irx_img[i].ptr, irx_img[i].size,
                                     (u32)arglen, arglen ? args : NULL, &result);
        } else {
            id = SifLoadStartModule(cfg.irx[i].path, arglen, arglen ? args : NULL, &result);
        }
        // Only id < 0 (could not load) is an error. The module's own start
        // result is not checked.
        if (id < 0) { dbg_irx_index = i + 1; return -1; }
    }
    return 0;
}

// Loads boot=. With irx= or reset=1 the storage device may still be
// coming up, so retry for ~5 s; otherwise a single attempt.
static int load_boot(t_ExecData *elf_data) {
    int tries = (cfg.nirx > 0 || cfg.reset) ? BOOT_RETRY_COUNT : 0;
    int attempt;

    for (attempt = 0; ; attempt++) {
        memset(elf_data, 0, sizeof(*elf_data));
        if (SifLoadElf(cfg.boot, elf_data) >= 0 && elf_data->epc != 0) return 0;
        if (attempt >= tries) return -1;
        usleep(BOOT_RETRY_US);
    }
}

// Resolves every patch against the boot ELF's layout first, and only then
// writes, so a bad patch never leaves RAM half-patched.
static int apply_patches(void) {
    CfgLayout layout;
    CfgPatch patch;
    int i;

    if (cfg.npatch == 0) return 0;

    if (elf_load_layout(cfg.boot, &layout) < 0) return -1;

    for (i = 0; i < cfg.npatch; i++) {
        if (cfg_patch_parse(cfg.patch[i], &patch) < 0) return -1;
        if (cfg_layout_resolve(&layout, patch.offset, patch.len, &patch_addr[i]) < 0) return -1;
    }

    for (i = 0; i < cfg.npatch; i++) {
        if (cfg_patch_parse(cfg.patch[i], &patch) < 0) return -1;
        memcpy((void *)UNCACHED_SEG(patch_addr[i]), patch.data, patch.len);
    }

    FlushCache(0);
    FlushCache(2);
    return 0;
}

int main(int argc, char *argv[]) {
    t_ExecData elf_data;
    long len;
    int exec_argc, i;

    // 1. Read the whole config and validate it. The only thing touched
    //    on the IOP side is the file read; nothing is reset or patched.
    SifInitRpc(0);

    if (build_config_path(argc, argv) < 0) return BAIL(0x0000FF);
    len = read_config(config_path);
    if (len < 0) return BAIL(0x0000FF);
    if (cfg_parse(cfg_buf, (size_t)len, &cfg) != CFG_OK) return BAIL(0x00FFFF);

    if (resolve_irx_paths() < 0) return BAIL(0x00FF00);

    // 1b. With reset=1, pull every non-rom0 irx= into RAM now: after the
    //     reset the launcher's devices (mass:, mc0:, ...) are gone.
    if (cfg.reset && read_irx_images() < 0) return BAIL(0x00FF00);

    // 2. Optional IOP reset, then RPC / loadfile / sbv patches.
    if (cfg.reset) reset_iop();

    SifLoadFileInit();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    // 3. IRX modules, in file order.
    if (load_irx_modules() < 0) return BAIL(0x00FF00);

    // 4. Load the target ELF into RAM.
    if (load_boot(&elf_data) < 0) return BAIL(0xFF0000);

    // 5. RAM patches + cache flush.
    if (apply_patches() < 0) return BAIL(0xFF00FF);

    // 6. Execute. argv[0] is argv0= if given, otherwise boot= unchanged.
    exec_argv[0] = cfg.argv0 ? cfg.argv0 : cfg.boot;
    for (i = 0; i < cfg.nargs; i++) exec_argv[1 + i] = cfg.args[i];
    exec_argc = 1 + cfg.nargs;
    exec_argv[exec_argc] = NULL;

    SifExitRpc();   // no IOP I/O after this point
    ExecPS2((void *)elf_data.epc, (void *)elf_data.gp, exec_argc, exec_argv);

    return -1;      // ExecPS2 only returns on failure
}
