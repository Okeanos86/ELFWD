/*
 * ELFWD - Generic PS2 ELF forwarder with argv spoofing and RAM patch
 *         support, driven by a plain-text config file.
 * Copyright (c) 2026 Okeanos
 *
 * Licensed under the Academic Free License version 2.0
 * See LICENSE for the full license text.
 *
 * ELFWD loads no IOP modules of its own: it relies entirely on the IOP
 * module set already resident from whichever launcher started it,
 * which is what makes it work from any storage device that launcher
 * already supports.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>

#define MAX_PATH 256
#define MAX_PATCH_VALUES 64
#define MAX_PHDRS 16

static char config_path[MAX_PATH * 2];

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

typedef struct {
    Elf32_Phdr phdrs[MAX_PHDRS];
    int count;
} ElfLayout;

static int open_launcher_same_dir(FILE **outf, int argc, char *argv[]) {
    char self[MAX_PATH];
    char *slash;

    if (argc <= 0 || !argv[0] || !argv[0][0]) return -1;

    strncpy(self, argv[0], sizeof(self) - 1);
    self[sizeof(self) - 1] = '\0';

    char *semi = strchr(self, ';');
    if (semi) *semi = '\0';

    slash = strrchr(self, '/');
    if (!slash) return -1;

    size_t dir_len = (size_t)(slash - self + 1);
    if (dir_len >= sizeof(config_path)) return -1;

    memcpy(config_path, self, dir_len);
    config_path[dir_len] = '\0';

    strncat(config_path, "ELFWD.CFG", sizeof(config_path) - strlen(config_path) - 1);

    *outf = fopen(config_path, "r");
    return (*outf != NULL) ? 0 : -1;
}

static char fake_argv0[MAX_PATH];
static char fake_argv1[MAX_PATH];
static char *exec_argv[3] = { NULL, NULL, NULL };

static void trim_eol(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static void strip_utf8_bom(char *s) {
    unsigned char *u = (unsigned char *)s;
    if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
        memmove(s, s + 3, strlen(s + 3) + 1);
    }
}

static void strip_hex_prefix(char **s) {
    char *p = *s;
    if (p[0] == '$') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    *s = p;
}

// Parses a hex token, validating that at least one valid hex digit was
// consumed AND that no trailing non-hex characters remain. Returns 0 on
// success (with *out set), -1 on a malformed token (empty string, no
// digits consumed, or garbage trailing the number) so callers can skip
// the line instead of silently treating garbage as offset/value 0 or
// truncating a token like "123G" down to "123".
static int parse_hex_strict(const char *s, uint32_t *out) {
    if (!s || s[0] == '\0') return -1;
    char *endptr = NULL;
    unsigned long v = strtoul(s, &endptr, 16);

    if (endptr == s || *endptr != '\0') return -1;

    *out = (uint32_t)v;
    return 0;
}

// If the last fgets() filled the buffer without reaching a newline, the
// physical line is longer than MAX_PATH and its tail is still unread.
// This drains the remainder so the next fgets() starts clean on the
// next real line, instead of reinterpreting the tail as a new directive.
static void discard_rest_of_line(FILE *f, const char *line_buf) {
    if (strchr(line_buf, '\n') != NULL) return;
    if (feof(f)) return;
    int c;
    while ((c = fgetc(f)) != '\n' && c != EOF) { }
}

// Reads zero or more optional directive lines right after argv0:
//   '@<value>'            -> argv1 for the target (has_argv1 set to 1)
//   '!RESET=0'/'!RESET=1'  -> reserved, parsed but not acted on
// Any other line (a patch line, a comment, or a blank line) stops the
// scan; the file position is rewound to the start of that line so
// apply_ram_patches() sees it as its own first line.
static int try_read_directives(FILE *f, long *out_patches_offset) {
    int has_argv1 = 0;

    for (;;) {
        long before = ftell(f);
        char line[MAX_PATH];

        if (!fgets(line, sizeof(line), f)) {
            *out_patches_offset = before;
            return has_argv1;
        }

        if (strchr(line, '\n') == NULL && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { }
        }

        trim_eol(line);

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (p[0] == '@' && !has_argv1) {
            char *val = p + 1;
            while (*val == ' ' || *val == '\t') val++;
            strncpy(fake_argv1, val, MAX_PATH - 1);
            fake_argv1[MAX_PATH - 1] = '\0';
            if (fake_argv1[0] != '\0') has_argv1 = 1;
            continue;
        }

        if (p[0] == '!') {
            // Recognized-but-inert directive prefix, reserved for future
            // options. Consumed and ignored rather than falling through
            // to the patch parser as a malformed offset.
            continue;
        }

        fseek(f, before, SEEK_SET);
        *out_patches_offset = before;
        return has_argv1;
    }
}

// Reads the ELF header and PT_LOAD program headers ONCE, to be reused
// for resolving every patch offset without reopening/rereading the file
// for each line.
static int elf_load_layout(const char *elf_path, ElfLayout *layout) {
    layout->count = 0;

    FILE *ef = fopen(elf_path, "rb");
    if (!ef) return -1;

    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, ef) != 1) { fclose(ef); return -1; }

    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F') {
        fclose(ef);
        return -1;
    }

    int n = eh.e_phnum;
    if (n > MAX_PHDRS) n = MAX_PHDRS;

    for (int i = 0; i < n; i++) {
        Elf32_Phdr ph;
        if (fseek(ef, eh.e_phoff + (uint32_t)i * eh.e_phentsize, SEEK_SET) != 0) break;
        if (fread(&ph, sizeof(ph), 1, ef) != 1) break;
        if (ph.p_type == PT_LOAD) {
            layout->phdrs[layout->count++] = ph;
        }
    }

    fclose(ef);
    return 0;
}

// Resolves a file offset to its RAM address using the already-loaded
// layout, no I/O involved.
static int layout_offset_to_vaddr(const ElfLayout *layout, uint32_t file_offset, uint32_t *out_vaddr) {
    for (int i = 0; i < layout->count; i++) {
        const Elf32_Phdr *ph = &layout->phdrs[i];
        if (file_offset >= ph->p_offset && file_offset < ph->p_offset + ph->p_filesz) {
            *out_vaddr = ph->p_vaddr + (file_offset - ph->p_offset);
            return 0;
        }
    }
    return -1;
}

// Line format: $OFFSET VALUE        (e.g. $412 0x01)
// or:          $OFFSET B1 B2 B3 ... (sequential single bytes)
static void apply_ram_patches(FILE *cfg_f, const ElfLayout *layout) {
    char line[MAX_PATH];
    int applied = 0;

    while (fgets(line, sizeof(line), cfg_f)) {
        int line_was_truncated = (strchr(line, '\n') == NULL) && !feof(cfg_f);

        trim_eol(line);

        char *cmt = strstr(line, "//");
        if (cmt) *cmt = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (p[0] == '\0' || p[0] == '#') {
            if (line_was_truncated) discard_rest_of_line(cfg_f, line);
            continue;
        }

        char *token = strtok(p, " \t");
        if (!token) {
            if (line_was_truncated) discard_rest_of_line(cfg_f, line);
            continue;
        }
        strip_hex_prefix(&token);

        uint32_t file_offset;
        if (parse_hex_strict(token, &file_offset) < 0) {
            while (strtok(NULL, " \t") != NULL) { }
            if (line_was_truncated) discard_rest_of_line(cfg_f, line);
            continue;
        }

        uint32_t addr;
        if (layout_offset_to_vaddr(layout, file_offset, &addr) < 0) {
            while (strtok(NULL, " \t") != NULL) { }
            if (line_was_truncated) discard_rest_of_line(cfg_f, line);
            continue;
        }

        char *values[MAX_PATCH_VALUES];
        int n = 0;
        char *t;
        while (n < MAX_PATCH_VALUES && (t = strtok(NULL, " \t")) != NULL) {
            values[n++] = t;
        }

        if (n == 0) {
            if (line_was_truncated) discard_rest_of_line(cfg_f, line);
            continue;
        }

        // Validate every value token before writing anything from this
        // line, so a malformed later token doesn't leave a partial write.
        int valid = 1;
        uint32_t parsed_values[MAX_PATCH_VALUES];
        for (int i = 0; i < n; i++) {
            char *v = values[i];
            strip_hex_prefix(&v);
            if (parse_hex_strict(v, &parsed_values[i]) < 0) { valid = 0; break; }
        }

        if (!valid) {
            if (line_was_truncated) discard_rest_of_line(cfg_f, line);
            continue;
        }

        uint8_t *mem_ptr = (uint8_t *)UNCACHED_SEG(addr);

        if (n == 1) {
            char *v = values[0];
            strip_hex_prefix(&v);
            int digits = (int)strlen(v);
            uint32_t value = parsed_values[0];
            int nbytes = (digits <= 2) ? 1 : (digits <= 4) ? 2 : 4;

            for (int i = 0; i < nbytes; i++) {
                mem_ptr[i] = (uint8_t)((value >> (8 * i)) & 0xFF);
            }
        } else {
            for (int i = 0; i < n; i++) {
                mem_ptr[i] = (uint8_t)parsed_values[i];
            }
        }
        applied = 1;

        if (line_was_truncated) discard_rest_of_line(cfg_f, line);
    }

    if (applied) {
        FlushCache(0);
        FlushCache(2);
    }
}

int main(int argc, char *argv[]) {
    char target_elf[MAX_PATH] = {0};
    t_ExecData elf_data;
    FILE *f = NULL;

    // 1. RPC setup.
    SifInitRpc(0);
    SifLoadFileInit();

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    // 2. Read configuration from ELFWD.CFG
    if (open_launcher_same_dir(&f, argc, argv) < 0 || !f) { SifExitRpc(); return -1; }

    if (!fgets(target_elf, sizeof(target_elf), f)) { fclose(f); SifExitRpc(); return -1; }
    trim_eol(target_elf);
    strip_utf8_bom(target_elf);

    if (target_elf[0] == '\0') { fclose(f); SifExitRpc(); return -1; }

    // Line 2 (argv0 override) is optional. If the next line looks like a
    // directive ('@' or '!') or is blank, it's NOT argv0 — rewind so
    // try_read_directives()/apply_ram_patches() sees it untouched, and
    // fall back to the real target_elf path for argv[0] below.
    long before_argv0 = ftell(f);
    if (fgets(fake_argv0, sizeof(fake_argv0), f)) {
        int argv0_line_truncated = (strchr(fake_argv0, '\n') == NULL) && !feof(f);
        if (argv0_line_truncated) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { }
        }

        trim_eol(fake_argv0);

        char *p = fake_argv0;
        while (*p == ' ' || *p == '\t') p++;

        if (p[0] == '@' || p[0] == '!' || p[0] == '\0') {
            fseek(f, before_argv0, SEEK_SET);
            fake_argv0[0] = '\0';
        }
    } else {
        fake_argv0[0] = '\0';
    }

    // Optional directive lines: "@<value>" for argv[1], "!..." reserved.
    // Anything else (a patch line, comment, or blank line) is left
    // untouched for apply_ram_patches.
    long patches_offset;
    int has_argv1 = try_read_directives(f, &patches_offset);

    fclose(f);
    f = NULL;

    // 3. Load the target ELF into RAM
    memset(&elf_data, 0, sizeof(t_ExecData));
    if (SifLoadElf(target_elf, &elf_data) < 0 || elf_data.epc == 0) {
        SifExitRpc();
        return -1;
    }

    // 4. Reopen ELFWD.CFG to read and apply any patches, resolving all
    //    offsets against a single read of the target ELF's layout.
    FILE *pf = fopen(config_path, "r");
    if (pf) {
        if (fseek(pf, patches_offset, SEEK_SET) == 0) {
            ElfLayout layout;
            if (elf_load_layout(target_elf, &layout) == 0) {
                apply_ram_patches(pf, &layout);
            }
        }
        fclose(pf);
    }

    // 5. Execution
    // argv[0]: the fake path from line 2 if one was given, otherwise the
    // real target_elf path unmodified.
    exec_argv[0] = (fake_argv0[0] != '\0') ? fake_argv0 : target_elf;

    int exec_argc = 1;
    if (has_argv1) {
        exec_argv[1] = fake_argv1;
        exec_argv[2] = NULL;
        exec_argc = 2;
    }

    SifExitRpc();
    ExecPS2((void *)elf_data.epc, (void *)elf_data.gp, exec_argc, exec_argv);

    return 0;
}
