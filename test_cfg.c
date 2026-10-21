/* Host test for cfg.c:  gcc -Wall -Wextra -o test_cfg test_cfg.c cfg.c && ./test_cfg */
#include <stdio.h>
#include <string.h>
#include "cfg.h"

static int fails, total;

static CfgErr run(const char *text, size_t n, Cfg *cfg, char *buf) {
    memcpy(buf, text, n);
    buf[n] = '\0';
    return cfg_parse(buf, n, cfg);
}

#define EXPECT(name, text, code) do { \
    static char b[CFG_MAX_FILE + 64]; Cfg c; \
    CfgErr e = run(text, sizeof(text) - 1, &c, b); \
    total++; \
    if (e != (code)) { fails++; printf("FAIL %-28s got %s expected %s\n", name, cfg_strerror(e), cfg_strerror(code)); } \
} while (0)

static void check(const char *name, int cond) {
    total++;
    if (!cond) { fails++; printf("FAIL %s\n", name); }
}

int main(void) {
    static char big[CFG_MAX_FILE + 64];
    static char b[CFG_MAX_FILE + 64];
    Cfg c;
    CfgPatch p;
    CfgLayout l;
    uint32_t a;
    char out[CFG_IRX_ARGS_MAX];
    size_t n;

    /* valid */
    EXPECT("minimal", "boot=mass:/A.ELF\n", CFG_OK);
    EXPECT("no final newline", "boot=mass:/A.ELF", CFG_OK);
    EXPECT("crlf", "boot=mass:/A.ELF\r\nreset=1\r\n", CFG_OK);
    EXPECT("bom", "\xEF\xBB\xBF" "boot=mass:/A.ELF\n", CFG_OK);
    EXPECT("case-insens + spaces", "  BOOT  =  mass:/A.ELF  \nArg = x y\n", CFG_OK);
    EXPECT("blank + comments", "\n# c\n   # c2\nboot=a\n\n", CFG_OK);
    EXPECT("full", "boot=a\nargv0=b\narg=1\narg=2\nreset=0\nirx=rom0:X a b\npatch=$412 0x01\npatch=10 1 2 3\n", CFG_OK);
    EXPECT("value keeps '='/#/quotes", "boot=a\narg=\"x=y # z\"\n", CFG_OK);

    /* errors */
    EXPECT("empty file", "", CFG_E_NO_BOOT);
    EXPECT("only comments", "# x\n", CFG_E_NO_BOOT);
    EXPECT("no equals", "boot=a\nfoo\n", CFG_E_NO_EQUALS);
    EXPECT("unknown key", "boot=a\nfoo=1\n", CFG_E_UNKNOWN_KEY);
    EXPECT("empty key", "boot=a\n=1\n", CFG_E_UNKNOWN_KEY);
    EXPECT("dup boot", "boot=a\nboot=b\n", CFG_E_DUPLICATE);
    EXPECT("dup argv0", "boot=a\nargv0=a\nargv0=b\n", CFG_E_DUPLICATE);
    EXPECT("dup reset", "boot=a\nreset=0\nreset=1\n", CFG_E_DUPLICATE);
    EXPECT("empty boot", "boot=\n", CFG_E_EMPTY_VALUE);
    EXPECT("blank-only value", "boot=a\narg=   \n", CFG_E_EMPTY_VALUE);
    EXPECT("empty patch", "boot=a\npatch=\n", CFG_E_EMPTY_VALUE);
    EXPECT("reset=2", "boot=a\nreset=2\n", CFG_E_BAD_RESET);
    EXPECT("reset=01", "boot=a\nreset=01\n", CFG_E_BAD_RESET);
    EXPECT("trailing comment on value", "boot=a\nreset=1 # x\n", CFG_E_BAD_RESET);
    EXPECT("patch no value", "boot=a\npatch=412\n", CFG_E_BAD_PATCH);
    EXPECT("patch garbage", "boot=a\npatch=412 12G\n", CFG_E_BAD_PATCH);
    EXPECT("patch >8 digits", "boot=a\npatch=412 123456789\n", CFG_E_BAD_PATCH);
    EXPECT("patch byte >FF", "boot=a\npatch=412 1 100\n", CFG_E_BAD_PATCH);
    EXPECT("patch bare 0x", "boot=a\npatch=412 0x\n", CFG_E_BAD_PATCH);
    EXPECT("patch 0x0x", "boot=a\npatch=412 0x0x1\n", CFG_E_BAD_PATCH);
    EXPECT("irx rooted /", "boot=a\nirx=/\n", CFG_E_BAD_IRX);
    EXPECT("irx rooted /x.irx", "boot=a\nirx=/x.irx\n", CFG_E_BAD_IRX);
    EXPECT("irx rooted backslash", "boot=a\nirx=\\x.irx\n", CFG_E_BAD_IRX);
    EXPECT("irx device no slash ok", "boot=a\nirx=mass:x.irx\nirx=sub/x.irx\n", CFG_OK);
    EXPECT("nul byte", "boot=a\n\0x=1\n", CFG_E_NUL_BYTE);
    EXPECT("boot missing", "arg=1\n", CFG_E_NO_BOOT);

    /* limits */
    {   /* line: 255 ok, 256 fail ("boot=" is 5 chars) */
        memset(big, 'a', sizeof(big));
        memcpy(big, "boot=", 5);
        big[255] = '\n';
        total++; if (cfg_parse(strcpy(b, ""), 0, &c) != CFG_E_NO_BOOT) { fails++; puts("FAIL setup"); }
        memcpy(b, big, 256); b[256] = 0;
        total++; if (cfg_parse(b, 256, &c) != CFG_OK) { fails++; puts("FAIL line 255 ok"); }
        big[255] = 'a'; big[256] = '\n';
        memcpy(b, big, 257); b[257] = 0;
        total++; if (cfg_parse(b, 257, &c) != CFG_E_LINE_TOO_LONG) { fails++; puts("FAIL line 256"); }
    }
    {   /* 12 args ok, 13 fail; 16 irx ok, 17 fail */
        char t[2048]; int i; n = 0;
        n += (size_t)sprintf(t + n, "boot=a\n");
        for (i = 0; i < 12; i++) n += (size_t)sprintf(t + n, "arg=x\n");
        memcpy(b, t, n); b[n] = 0; check("12 args", cfg_parse(b, n, &c) == CFG_OK);
        n += (size_t)sprintf(t + n, "arg=x\n");
        memcpy(b, t, n); b[n] = 0; check("13 args", cfg_parse(b, n, &c) == CFG_E_TOO_MANY);
        n = (size_t)sprintf(t, "boot=a\n");
        for (i = 0; i < 16; i++) n += (size_t)sprintf(t + n, "irx=m%d\n", i);
        memcpy(b, t, n); b[n] = 0; check("16 irx", cfg_parse(b, n, &c) == CFG_OK);
        n += (size_t)sprintf(t + n, "irx=z\n");
        memcpy(b, t, n); b[n] = 0; check("17 irx", cfg_parse(b, n, &c) == CFG_E_TOO_MANY);
    }
    {   /* 64 patch values ok, 65 fail */
        char t[1024]; int i; n = (size_t)sprintf(t, "boot=a\npatch=10");
        for (i = 0; i < 64; i++) n += (size_t)sprintf(t + n, " 1");
        n += (size_t)sprintf(t + n, "\n");
        memcpy(b, t, n); b[n] = 0; check("64 values", cfg_parse(b, n, &c) == CFG_OK);
        n--; n += (size_t)sprintf(t + n, " 1\n");
        memcpy(b, t, n); b[n] = 0; check("65 values", cfg_parse(b, n, &c) == CFG_E_BAD_PATCH);
    }
    {   /* file size: exactly 16384 ok, 16385 fail; argv total 2048 */
        size_t i;
        memset(big, '#', CFG_MAX_FILE + 1);
        memcpy(big, "boot=a\n", 7);
        for (i = 100; i < CFG_MAX_FILE + 1; i += 100) big[i] = '\n';
        memcpy(b, big, CFG_MAX_FILE); b[CFG_MAX_FILE] = 0;
        check("file 16384", cfg_parse(b, CFG_MAX_FILE, &c) == CFG_OK);
        memcpy(b, big, CFG_MAX_FILE + 1); b[CFG_MAX_FILE + 1] = 0;
        check("file 16385", cfg_parse(b, CFG_MAX_FILE + 1, &c) == CFG_E_TOO_BIG);
    }
    {   /* argv: boot "a"(2 bytes as argv0) + 8 args of 255 ("arg=" + 251) ... compute exact */
        char t[4096]; int i; n = (size_t)sprintf(t, "boot=a\n"); /* argv0 = "a" -> 2 bytes */
        for (i = 0; i < 7; i++) { memset(t + n, 0, 1); n += (size_t)sprintf(t + n, "arg=%0250d\n", 7); } /* 250+1 each = 251 */
        /* 2 + 7*251 = 1759; add one arg of 288? lines are limited to 255 so use argv0 too */
        n += (size_t)sprintf(t + n, "argv0=%0249d\n", 1);   /* replaces effective argv0: 251 */
        /* total = 251 + 7*251 = 2008 */
        memcpy(b, t, n); b[n] = 0; check("argv 2007 ok", cfg_parse(b, n, &c) == CFG_OK && c.argv_bytes == 2007);
        n += (size_t)sprintf(t + n, "arg=%0250d\n", 7);     /* 2259 */
        memcpy(b, t, n); b[n] = 0; check("argv 2258 fail", cfg_parse(b, n, &c) == CFG_E_ARGV_SIZE);
    }

    /* parsed content */
    {
        static char t[] = "boot=mass:/A.ELF\nargv0=mass:/B.ELF\narg=\"q w\"\narg=2\nreset=1\nirx=rom0:SIO2MAN  a  b\tc\nirx=x.irx\n";
        n = strlen(t);
        check("parse full", cfg_parse(t, n, &c) == CFG_OK);
        check("boot", !strcmp(c.boot, "mass:/A.ELF"));
        check("argv0", c.argv0 && !strcmp(c.argv0, "mass:/B.ELF"));
        check("args verbatim", c.nargs == 2 && !strcmp(c.args[0], "\"q w\"") && !strcmp(c.args[1], "2"));
        check("reset", c.reset == 1);
        check("irx path/args", c.nirx == 2 && !strcmp(c.irx[0].path, "rom0:SIO2MAN") && !strcmp(c.irx[0].args, "a  b\tc") && c.irx[1].args == NULL);
        check("irx args block", cfg_irx_args(&c.irx[0], out, sizeof(out)) == 6 && !memcmp(out, "a\0b\0c\0", 6));
        check("irx no args", cfg_irx_args(&c.irx[1], out, sizeof(out)) == 0);
        check("irx cap", cfg_irx_args(&c.irx[0], out, 5) == -1);
    }
    {
        static char t[] = "boot=a\n";
        cfg_parse(t, strlen(t), &c);
        check("argv0 absent", c.argv0 == NULL && c.reset == 0 && c.argv_bytes == 2);
    }

    /* patch semantics */
    check("p1 byte", cfg_patch_parse("$412 0x01", &p) == 0 && p.offset == 0x412 && p.len == 1 && p.data[0] == 1);
    check("p2 bytes LE", cfg_patch_parse("412 1234", &p) == 0 && p.len == 2 && p.data[0] == 0x34 && p.data[1] == 0x12);
    check("p4 bytes LE", cfg_patch_parse("412 $12345678", &p) == 0 && p.len == 4 && p.data[0] == 0x78 && p.data[3] == 0x12);
    check("p3 digits->2 bytes", cfg_patch_parse("0 123", &p) == 0 && p.len == 2 && p.data[0] == 0x23 && p.data[1] == 0x01);
    check("p seq bytes", cfg_patch_parse("$10 $1 0x2 03", &p) == 0 && p.len == 3 && p.data[2] == 3);
    check("p tabs", cfg_patch_parse("\t10\t5\t6", &p) == 0 && p.len == 2);

    /* rom0: detection */
    check("rom0 prefix", cfg_path_is_rom("rom0:SIO2MAN") && cfg_path_is_rom("ROM0:X"));
    check("not rom0", !cfg_path_is_rom("mass:/a.irx") && !cfg_path_is_rom("rom0") && !cfg_path_is_rom("rom1:x") && !cfg_path_is_rom(""));

    /* irx path resolution against the launcher directory */
    {
        char o[48];
        check("name -> dir+name", cfg_resolve_irx_path("mass:/APPS/", "usbd.irx", o, sizeof(o)) == 0 && !strcmp(o, "mass:/APPS/usbd.irx"));
        check("subdir -> dir+path", cfg_resolve_irx_path("mass:/APPS/", "sub/x.irx", o, sizeof(o)) == 0 && !strcmp(o, "mass:/APPS/sub/x.irx"));
        check("backslash subdir", cfg_resolve_irx_path("mass:/APPS/", "sub\\x.irx", o, sizeof(o)) == 0 && !strcmp(o, "mass:/APPS/sub\\x.irx"));
        check("device path kept", cfg_resolve_irx_path("mass:/APPS/", "mc0:x.irx", o, sizeof(o)) == 0 && !strcmp(o, "mc0:x.irx"));
        check("device, no slash kept", cfg_resolve_irx_path("mass:/APPS/", "mass:file.irx", o, sizeof(o)) == 0 && !strcmp(o, "mass:file.irx"));
        check("rom0 kept", cfg_resolve_irx_path("mass:/APPS/", "rom0:SIO2MAN", o, sizeof(o)) == 0 && !strcmp(o, "rom0:SIO2MAN"));
        check("rooted kept", cfg_resolve_irx_path("mass:/APPS/", "/x.irx", o, sizeof(o)) == 0 && !strcmp(o, "/x.irx"));
        check("too long", cfg_resolve_irx_path("mass:/APPS/", "usbd.irx", o, 10) == -1);
    }

    /* layout */
    l.count = 2;
    l.seg[0] = (CfgSeg){ 0x100, 0x00100000, 0x1000 };
    l.seg[1] = (CfgSeg){ 0x2000, 0x00300000, 0x10 };
    check("res in seg0", cfg_layout_resolve(&l, 0x412, 2, &a) == 0 && a == 0x00100312);
    check("res seg1 start", cfg_layout_resolve(&l, 0x2000, 16, &a) == 0 && a == 0x00300000);
    check("res crosses end", cfg_layout_resolve(&l, 0x200F, 2, &a) < 0);
    check("res before seg", cfg_layout_resolve(&l, 0x50, 1, &a) < 0);
    check("res gap", cfg_layout_resolve(&l, 0x1100, 1, &a) < 0);
    check("res last byte", cfg_layout_resolve(&l, 0x10FF, 1, &a) == 0);
    check("res zero len", cfg_layout_resolve(&l, 0x412, 0, &a) < 0);

    printf("%d checks, %d failed\n", total, fails);
    return fails != 0;
}
