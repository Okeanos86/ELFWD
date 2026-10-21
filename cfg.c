/*
 * ELFWD - ELFWD.CFG parser (pure C, no PS2 dependencies: host-testable)
 * Copyright (c) 2026 Okeanos
 *
 * Licensed under the Academic Free License version 2.0
 * See LICENSE for the full license text.
 */
#include <string.h>
#include "cfg.h"

static int is_ws(char c) { return c == ' ' || c == '\t'; }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int key_is(const char *key, size_t klen, const char *name) {
    size_t i;
    for (i = 0; i < klen; i++) {
        if (name[i] == '\0' || lower(key[i]) != name[i]) return 0;
    }
    return name[klen] == '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accepts "$", "0x"/"0X" or no prefix, then 1..8 hex digits and nothing else. */
static int parse_hex_token(const char *s, size_t len, uint32_t *out, int *ndigits) {
    uint32_t v = 0;
    size_t i;

    if (len > 0 && s[0] == '$') { s++; len--; }
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; len -= 2; }
    if (len == 0 || len > 8) return -1;

    for (i = 0; i < len; i++) {
        int d = hexval(s[i]);
        if (d < 0) return -1;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    *ndigits = (int)len;
    return 0;
}

int cfg_patch_parse(const char *value, CfgPatch *out) {
    const char *p = value;
    const char *tok;
    uint32_t vals[CFG_MAX_PATCH_VALUES];
    int digits[CFG_MAX_PATCH_VALUES];
    int n = 0, dummy;
    uint32_t offset;

    while (is_ws(*p)) p++;
    tok = p;
    while (*p && !is_ws(*p)) p++;
    if (p == tok) return -1;
    if (parse_hex_token(tok, (size_t)(p - tok), &offset, &dummy) < 0) return -1;

    for (;;) {
        while (is_ws(*p)) p++;
        if (*p == '\0') break;
        tok = p;
        while (*p && !is_ws(*p)) p++;
        if (n >= CFG_MAX_PATCH_VALUES) return -1;
        if (parse_hex_token(tok, (size_t)(p - tok), &vals[n], &digits[n]) < 0) return -1;
        n++;
    }
    if (n == 0) return -1;

    out->offset = offset;
    if (n == 1) {
        /* cheat-style: size from digit count, little-endian */
        int nbytes = (digits[0] <= 2) ? 1 : (digits[0] <= 4) ? 2 : 4;
        int i;
        for (i = 0; i < nbytes; i++) out->data[i] = (uint8_t)((vals[0] >> (8 * i)) & 0xFF);
        out->len = (uint32_t)nbytes;
    } else {
        int i;
        for (i = 0; i < n; i++) {
            if (vals[i] > 0xFF) return -1;
            out->data[i] = (uint8_t)vals[i];
        }
        out->len = (uint32_t)n;
    }
    return 0;
}

int cfg_layout_resolve(const CfgLayout *l, uint32_t offset, uint32_t len, uint32_t *out_vaddr) {
    int i;
    if (len == 0) return -1;
    for (i = 0; i < l->count; i++) {
        const CfgSeg *s = &l->seg[i];
        if (offset >= s->offset) {
            uint32_t delta = offset - s->offset;
            if (delta < s->filesz && len <= s->filesz - delta) {
                *out_vaddr = s->vaddr + delta;
                return 0;
            }
        }
    }
    return -1;
}

int cfg_irx_args(const CfgIrx *irx, char *out, size_t cap) {
    const char *p = irx->args;
    size_t n = 0;

    if (!p) return 0;
    for (;;) {
        const char *tok;
        while (is_ws(*p)) p++;
        if (*p == '\0') break;
        tok = p;
        while (*p && !is_ws(*p)) p++;
        if (n + (size_t)(p - tok) + 1 > cap) return -1;
        memcpy(out + n, tok, (size_t)(p - tok));
        n += (size_t)(p - tok);
        out[n++] = '\0';
    }
    return (int)n;
}

int cfg_path_is_rom(const char *path) {
    static const char pfx[] = "rom0:";
    size_t i;
    for (i = 0; pfx[i]; i++) {
        if (path[i] == '\0' || lower(path[i]) != pfx[i]) return 0;
    }
    return 1;
}

int cfg_resolve_irx_path(const char *dir, const char *path, char *out, size_t cap) {
    size_t dl = 0, pl = strlen(path);
    int relative = (path[0] != '/' && path[0] != '\\');
    const char *q;

    for (q = path; *q && relative; q++) {
        if (*q == ':') relative = 0;     /* has a device: IOP resolves it */
    }
    if (relative && dir) dl = strlen(dir);
    if (dl + pl + 1 > cap) return -1;
    memcpy(out, dir ? dir : "", dl);
    memcpy(out + dl, path, pl + 1);
    return 0;
}

#define FAIL(code) do { cfg->err_line = lineno; return (code); } while (0)

CfgErr cfg_parse(char *buf, size_t len, Cfg *cfg) {
    size_t pos = 0;
    int lineno = 0;
    int have_boot = 0, have_argv0 = 0, have_reset = 0;
    size_t i;

    memset(cfg, 0, sizeof(*cfg));

    if (len > CFG_MAX_FILE) return CFG_E_TOO_BIG;
    for (i = 0; i < len; i++) {
        if (buf[i] == '\0') return CFG_E_NUL_BYTE;
    }
    buf[len] = '\0';

    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        pos = 3;
    }

    while (pos < len) {
        size_t start = pos, end, kend;
        char *p, *key, *val, *eq, *vend;
        size_t klen;

        lineno++;
        while (pos < len && buf[pos] != '\n') pos++;
        end = pos;
        if (pos < len) pos++;                       /* skip '\n' */
        if (end > start && buf[end - 1] == '\r') end--;

        if (end - start > CFG_MAX_LINE) FAIL(CFG_E_LINE_TOO_LONG);
        buf[end] = '\0';

        p = buf + start;
        while (is_ws(*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        eq = strchr(p, '=');
        if (!eq) FAIL(CFG_E_NO_EQUALS);

        key = p;
        kend = (size_t)(eq - key);
        while (kend > 0 && is_ws(key[kend - 1])) kend--;
        klen = kend;

        val = eq + 1;
        while (is_ws(*val)) val++;
        vend = buf + end;
        while (vend > val && is_ws(vend[-1])) vend--;
        *vend = '\0';

        if (klen == 0) FAIL(CFG_E_UNKNOWN_KEY);

        if (key_is(key, klen, "boot")) {
            if (have_boot) FAIL(CFG_E_DUPLICATE);
            if (*val == '\0') FAIL(CFG_E_EMPTY_VALUE);
            cfg->boot = val; have_boot = 1;
        } else if (key_is(key, klen, "argv0")) {
            if (have_argv0) FAIL(CFG_E_DUPLICATE);
            if (*val == '\0') FAIL(CFG_E_EMPTY_VALUE);
            cfg->argv0 = val; have_argv0 = 1;
        } else if (key_is(key, klen, "reset")) {
            if (have_reset) FAIL(CFG_E_DUPLICATE);
            if (*val == '\0') FAIL(CFG_E_EMPTY_VALUE);
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') FAIL(CFG_E_BAD_RESET);
            cfg->reset = val[0] - '0'; have_reset = 1;
        } else if (key_is(key, klen, "arg")) {
            if (*val == '\0') FAIL(CFG_E_EMPTY_VALUE);
            if (cfg->nargs >= CFG_MAX_ARGS) FAIL(CFG_E_TOO_MANY);
            cfg->args[cfg->nargs++] = val;
        } else if (key_is(key, klen, "irx")) {
            CfgIrx *m;
            char *sp;
            if (*val == '\0') FAIL(CFG_E_EMPTY_VALUE);
            if (cfg->nirx >= CFG_MAX_IRX) FAIL(CFG_E_TOO_MANY);
            if (*val == '/' || *val == '\\') FAIL(CFG_E_BAD_IRX);
            m = &cfg->irx[cfg->nirx++];
            m->path = val;
            m->args = NULL;
            sp = val;
            while (*sp && !is_ws(*sp)) sp++;
            if (*sp) {
                *sp++ = '\0';
                while (is_ws(*sp)) sp++;
                if (*sp) m->args = sp;
            }
        } else if (key_is(key, klen, "patch")) {
            CfgPatch tmp;
            if (*val == '\0') FAIL(CFG_E_EMPTY_VALUE);
            if (cfg->npatch >= CFG_MAX_PATCHES) FAIL(CFG_E_TOO_MANY);
            if (cfg_patch_parse(val, &tmp) < 0) FAIL(CFG_E_BAD_PATCH);
            cfg->patch[cfg->npatch++] = val;
        } else {
            FAIL(CFG_E_UNKNOWN_KEY);
        }
    }

    cfg->err_line = 0;
    if (!have_boot) return CFG_E_NO_BOOT;

    cfg->argv_bytes = strlen(cfg->argv0 ? cfg->argv0 : cfg->boot) + 1;
    for (i = 0; i < (size_t)cfg->nargs; i++) cfg->argv_bytes += strlen(cfg->args[i]) + 1;
    if (cfg->argv_bytes > CFG_MAX_ARGV_BYTES) return CFG_E_ARGV_SIZE;

    return CFG_OK;
}

const char *cfg_strerror(CfgErr e) {
    switch (e) {
    case CFG_OK:              return "ok";
    case CFG_E_TOO_BIG:       return "file too big";
    case CFG_E_NUL_BYTE:      return "NUL byte in file";
    case CFG_E_LINE_TOO_LONG: return "line too long";
    case CFG_E_NO_EQUALS:     return "line without '='";
    case CFG_E_UNKNOWN_KEY:   return "unknown key";
    case CFG_E_DUPLICATE:     return "duplicate key";
    case CFG_E_EMPTY_VALUE:   return "empty value";
    case CFG_E_BAD_RESET:     return "reset= must be 0 or 1";
    case CFG_E_TOO_MANY:      return "too many entries";
    case CFG_E_BAD_PATCH:     return "malformed patch";
    case CFG_E_BAD_IRX:       return "irx path starts with '/' or '\\'";
    case CFG_E_NO_BOOT:       return "boot= missing";
    case CFG_E_ARGV_SIZE:     return "argv over size limit";
    }
    return "?";
}
