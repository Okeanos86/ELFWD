/*
 * ELFWD - ELFWD.CFG parser (pure C, no PS2 dependencies: host-testable)
 * Copyright (c) 2026 Okeanos
 *
 * Licensed under the Academic Free License version 2.0
 * See LICENSE for the full license text.
 */
#ifndef ELFWD_CFG_H
#define ELFWD_CFG_H

#include <stddef.h>
#include <stdint.h>

#define CFG_MAX_LINE          255   /* characters per line, EOL excluded   */
#define CFG_MAX_FILE          16384 /* bytes                               */
#define CFG_MAX_ARGS          12    /* arg= entries                        */
#define CFG_MAX_IRX           16    /* irx= entries                        */
#define CFG_MAX_PATCH_VALUES  64    /* tokens after the offset in patch=   */
#define CFG_MAX_ARGV_BYTES    2048  /* argv0 + arg= strings, NULs included */
#define CFG_MAX_PATCHES       2048  /* far above what 16 KB can hold       */
#define CFG_MAX_SEGS          16    /* PT_LOAD segments tracked            */
#define CFG_IRX_ARGS_MAX      (CFG_MAX_LINE + 2)
#define CFG_MAX_IRX_BYTES     (512 * 1024) /* irx images held in RAM (reset=1) */

typedef enum {
    CFG_OK = 0,
    CFG_E_TOO_BIG,       /* file > 16 KB                                   */
    CFG_E_NUL_BYTE,      /* embedded NUL in the file                       */
    CFG_E_LINE_TOO_LONG, /* line > 255 characters                          */
    CFG_E_NO_EQUALS,     /* non-blank, non-comment line without '='        */
    CFG_E_UNKNOWN_KEY,   /* unknown (or empty) key                         */
    CFG_E_DUPLICATE,     /* boot=, argv0= or reset= repeated               */
    CFG_E_EMPTY_VALUE,   /* any key with an empty value                    */
    CFG_E_BAD_RESET,     /* reset= not exactly 0 or 1                      */
    CFG_E_TOO_MANY,      /* arg= > 12, irx= > 16, patch= > table size      */
    CFG_E_BAD_PATCH,     /* malformed patch= line                          */
    CFG_E_BAD_IRX,       /* irx= path starting with '/' or '\\' (no device) */
    CFG_E_NO_BOOT,       /* boot= missing                                  */
    CFG_E_ARGV_SIZE      /* argv0 + arg= over 2048 bytes                   */
} CfgErr;

typedef struct {
    char *path;          /* NUL-terminated, no spaces                      */
    char *args;          /* NULL, or the text after the first whitespace   */
} CfgIrx;

typedef struct {
    char       *boot;                        /* required                  */
    char       *argv0;                       /* NULL if absent            */
    int         reset;                       /* 0 or 1                    */
    int         nargs;
    char       *args[CFG_MAX_ARGS];
    int         nirx;
    CfgIrx      irx[CFG_MAX_IRX];
    int         npatch;
    const char *patch[CFG_MAX_PATCHES];      /* validated value strings   */
    size_t      argv_bytes;                  /* effective argv0 + args    */
    int         err_line;                    /* 1-based, 0 if n/a         */
} Cfg;

/* One validated patch: bytes to write at a file offset of the boot ELF. */
typedef struct {
    uint32_t offset;
    uint32_t len;                            /* 1..64                     */
    uint8_t  data[CFG_MAX_PATCH_VALUES];
} CfgPatch;

typedef struct {
    uint32_t offset, vaddr, filesz;
} CfgSeg;

typedef struct {
    CfgSeg seg[CFG_MAX_SEGS];
    int    count;
} CfgLayout;

/*
 * Parses and validates a whole file already in memory. `buf` must hold
 * `len` bytes plus a terminating NUL at buf[len]; it is modified in place
 * and the pointers in `cfg` point into it, so it must outlive `cfg`.
 * No side effects other than writing to buf/cfg.
 */
CfgErr cfg_parse(char *buf, size_t len, Cfg *cfg);

/* Parses one patch= value (no key). Returns 0 on success, -1 if malformed. */
int cfg_patch_parse(const char *value, CfgPatch *out);

/*
 * Resolves [offset, offset+len) of the ELF file to a RAM address. The whole
 * range must lie inside the file-backed part of a single PT_LOAD segment.
 * Returns 0 on success, -1 otherwise.
 */
int cfg_layout_resolve(const CfgLayout *l, uint32_t offset, uint32_t len,
                       uint32_t *out_vaddr);

/*
 * Converts an irx= argument string to the NUL-separated block expected by
 * SifLoadStartModule(). Returns the block length (trailing NUL included),
 * 0 if there are no arguments, -1 if it does not fit in `cap`.
 */
int cfg_irx_args(const CfgIrx *irx, char *out, size_t cap);

/* Non-zero if the path starts with "rom0:" (case-insensitive). Such modules
 * live in the IOP ROM and survive a reset, so they are never read into RAM. */
int cfg_path_is_rom(const char *path);

/*
 * An irx= path with no device (no ':') and not starting with '/' or '\\' is
 * resolved against `dir` (the launcher's directory, trailing slash included),
 * so both "x.irx" and "sub/x.irx" mean "relative to the launcher". Paths with
 * a device ("mass:x.irx", "rom0:SIO2MAN") and rooted paths are copied unchanged.
 * Returns 0, or -1 if the result does not fit in `cap`.
 */
int cfg_resolve_irx_path(const char *dir, const char *path, char *out, size_t cap);

const char *cfg_strerror(CfgErr e);

#endif
