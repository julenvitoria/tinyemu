/*
 * host_settings.c -- TIER-1 GRADER FOR THE PER-TITLE SETTINGS FORMAT.
 *
 * Runs NATIVELY, with no target, no emulator and no SD card, because
 * x86_settings.c and x86_cfg.c deliberately do no I/O and depend on nothing but
 * string.h and one printf. That property is the entire reason the prior port's
 * two-layer split was worth keeping, and this file is what cashes it in.
 *
 * The runner is tests/test_settings.py. Everything asserted lives here.
 *
 * WHAT IS GRADED
 *   A. defaults              -- every setting reads its compiled-in default,
 *                               including through the lazy-reset path
 *   B. ranges                -- set()/base_set() refuse out of range, and do
 *                               not store what they refused
 *   C. round trip            -- set a value, save, load, get it back
 *   D. only non-defaults     -- an unchanged table serialises to an empty file
 *   E. corrupt file          -- bad magic / bad CRC / bad version / too many
 *   F. truncated file        -- a count that outruns the bytes handed over
 *   G. unknown version       -- refused by name, table still usable
 *   H. forward compatibility -- an unknown pair id is CARRIED, not dropped
 *   I. the .cfg text layer   -- keys, values, comments, CRLF, quoting, the
 *                               binary sniff, the size cap, unknown keys
 *   J. the three layers      -- default < .cfg < .dosset, and the .cfg moving
 *                               a title that never overrode by hand
 *   K. strings               -- entry/args, empty-vs-unset, refusal not
 *                               truncation
 *   L. the shipped .cfg files-- the real ones on the card parse to "no change"
 *
 * NEGATIVE CONTROLS. Built with -DX86_SETTINGS_CTL_* / -DX86_CFG_CTL_*, each of
 * which switches one guard off. Every one of them MUST make this program fail,
 * and the runner requires it. A control that passes is a check that was never
 * checking.
 */

#include <stdio.h>
#include <string.h>

#include "x86_settings.h"
#include "x86_cfg.h"

static int failures;
static int checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void ck_eq(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL: %s -- got %d, want %d\n", what, got, want);
    }
}

static void ck_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL: %s -- got '%s', want '%s'\n", what, got, want);
    }
}

/* Parse a NUL-terminated .cfg literal. Every test that follows goes through
   this rather than calling parse() with a hand-counted length, so a test can
   never disagree with itself about how long its own input is. */
static x86_cfg_status_t cfg(const char *text, x86_cfg_result_t *r)
{
    return x86_cfg_parse((const unsigned char *)text,
                         (unsigned long)strlen(text), r);
}

/* ---- A. defaults --------------------------------------------------------- */

static void test_defaults(void)
{
    x86_settings_base_clear();
    x86_settings_reset();

    /* THE VALUES THIS CORE HAD BEFORE ANY OF THIS EXISTED. If one of these ever
       has to change, that is a behaviour change across every title at once and
       it should cost somebody a deliberate edit here. */
    ck_eq(x86_settings_builtin(X86_SET_CPU_PROFILE), 0, "builtin cpu_profile");
    ck_eq(x86_settings_builtin(X86_SET_USE_CMDTAIL), 0, "builtin use_cmdtail");
    ck_eq(x86_settings_builtin(X86_SET_XMS_KB),      0, "builtin xms_kb");
    ck_eq(x86_settings_builtin(X86_SET_EMS_KB),      0, "builtin ems_kb");
    ck_eq(x86_settings_builtin(X86_SET_CPU_CYCLES),  0, "builtin cpu_cycles");

    /* "UNSPECIFIED" IS base_is_set(), NOT THE VALUE 0 -- which is what lets
       `xms = false` mean something different from saying nothing. */
    for (int i = 0; i < X86_SET_COUNT; i++)
        ck_eq(x86_settings_base_is_set((x86_setting_id_t)i), 0,
              "with no .cfg, nothing is 'specified'");

    for (int i = 0; i < X86_SET_COUNT; i++)
        ck_eq(x86_settings_get((x86_setting_id_t)i),
              x86_settings_builtin((x86_setting_id_t)i),
              "reset leaves every setting at its builtin");

    /* Out-of-range ids cannot fault and answer 0. */
    ck_eq(x86_settings_get((x86_setting_id_t)X86_SET_COUNT), 0, "get(past end)");
    ck_eq(x86_settings_get((x86_setting_id_t)-1), 0, "get(negative)");
    ck_str(x86_settings_name((x86_setting_id_t)X86_SET_COUNT), "?", "name(past end)");

    /* Every setting has a distinct, non-empty name -- the diagnostic is useless
       if two rows print the same word. */
    for (int i = 0; i < X86_SET_COUNT; i++) {
        const char *a = x86_settings_name((x86_setting_id_t)i);
        ck(a[0] != 0, "setting name is non-empty");
        for (int j = i + 1; j < X86_SET_COUNT; j++)
            ck(strcmp(a, x86_settings_name((x86_setting_id_t)j)) != 0,
               "setting names are distinct");
    }
}

/* ---- B. ranges ----------------------------------------------------------- */

static void test_ranges(void)
{
    int lo = -1, hi = -1;

    x86_settings_base_clear();
    x86_settings_reset();

    ck_eq(x86_settings_range(X86_SET_CPU_PROFILE, &lo, &hi), 1, "range() ok");
    ck_eq(lo, 0, "cpu_profile lo");
    ck_eq(hi, 15, "cpu_profile hi");
    ck_eq(x86_settings_range((x86_setting_id_t)X86_SET_COUNT, &lo, &hi), 0,
          "range(past end) refused");
    ck_eq(lo, 0, "range(past end) left *lo alone");

    /* set() REFUSES rather than clamps, and does not store what it refused. */
    x86_settings_set(X86_SET_CPU_PROFILE, 5);
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 5, "in-range set took");
    x86_settings_set(X86_SET_CPU_PROFILE, 99);
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 5, "out-of-range set refused");
    x86_settings_set(X86_SET_CPU_PROFILE, -1);
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 5, "negative set refused");

    /* base_set() the same, and it reports rather than storing. */
    ck_eq(x86_settings_base_set(X86_SET_EMS_KB, 2048), 0, "base_set past hi refused");
    ck_eq(x86_settings_base_is_set(X86_SET_EMS_KB), 0, "refused base did not latch");
    ck_eq(x86_settings_base_set(X86_SET_EMS_KB, 256), 1, "base_set in range");
    ck_eq(x86_settings_base_is_set(X86_SET_EMS_KB), 1, "accepted base latched");
    x86_settings_base_clear();
    ck_eq(x86_settings_base_is_set(X86_SET_EMS_KB), 0, "base_clear cleared it");
}

/* ---- C/D. round trip, and only non-defaults ------------------------------ */

static void test_roundtrip(void)
{
    unsigned char buf[X86_SETTINGS_MAX_BYTES];
    unsigned long n;

    x86_settings_base_clear();
    x86_settings_reset();

    /* D. Nothing changed -> header + zero count + CRC, and NOTHING else. */
    n = x86_settings_save(buf, sizeof buf);
    ck_eq((int)n, X86_SETTINGS_HDR_BYTES + 4, "all-default save is an empty file");
    ck_eq((int)buf[8], 0, "all-default save has count 0");
    ck_eq(x86_settings_dirty(), 0, "nothing set -> not dirty");

    /* A short buffer is refused rather than half-filled. */
    ck_eq((int)x86_settings_save(buf, X86_SETTINGS_MAX_BYTES - 1), 0,
          "save refuses a buffer it cannot fill");

    /* C. Set, save, load, get it back. THE WHOLE POINT OF THE FEATURE. */
    x86_settings_set(X86_SET_CPU_PROFILE, 7);
    x86_settings_set(X86_SET_USE_CMDTAIL, 1);
    x86_settings_set(X86_SET_XMS_KB, 2048);
    ck_eq(x86_settings_dirty(), 1, "changed -> dirty");

    n = x86_settings_save(buf, sizeof buf);
    ck_eq((int)n, X86_SETTINGS_HDR_BYTES + 3 * X86_SETTINGS_PAIR_BYTES + 4,
          "three non-defaults -> three pairs");

    /* Clobber the live table first, so a load that does nothing at all cannot
       pass this by leaving the values we just set sitting there. */
    x86_settings_set(X86_SET_CPU_PROFILE, 1);
    x86_settings_set(X86_SET_USE_CMDTAIL, 0);
    x86_settings_set(X86_SET_XMS_KB, 64);

    ck_eq((int)x86_settings_load(buf, n), X86_SETTINGS_OK, "reload ok");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 7, "round trip cpu_profile");
    ck_eq(x86_settings_get(X86_SET_USE_CMDTAIL), 1, "round trip use_cmdtail");
    ck_eq(x86_settings_get(X86_SET_XMS_KB), 2048, "round trip xms_kb");
    ck_eq(x86_settings_get(X86_SET_EMS_KB), 0, "untouched setting still default");
    ck_eq(x86_settings_dirty(), 0, "straight after a load, not dirty");

    /* Saving the same state twice produces the same bytes -- what lets a test
       compare files instead of parsing them. */
    {
        unsigned char b2[X86_SETTINGS_MAX_BYTES];
        unsigned long n2 = x86_settings_save(b2, sizeof b2);
        ck_eq((int)n2, (int)n, "re-save is the same length");
        ck(memcmp(buf, b2, (size_t)n) == 0, "re-save is byte-identical");
    }

    /* OVER-READ IS LEGAL. FatFs hands back whole bytes read and src/main.c
       reads into a fixed buffer, so load() must accept len > need. */
    ck_eq((int)x86_settings_load(buf, sizeof buf), X86_SETTINGS_OK,
          "load accepts a buffer longer than the file");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 7, "over-read still parsed");
}

/* ---- E/F/G. corrupt, truncated, unknown version -------------------------- */

static void test_corrupt(void)
{
    unsigned char buf[X86_SETTINGS_MAX_BYTES];
    unsigned char bad[X86_SETTINGS_MAX_BYTES];
    unsigned long n;

    x86_settings_base_clear();
    x86_settings_reset();
    x86_settings_set(X86_SET_CPU_PROFILE, 6);
    n = x86_settings_save(buf, sizeof buf);

    /* Absent: zero bytes, and NOT an error. */
    ck_eq((int)x86_settings_load(0, 0), X86_SETTINGS_EABSENT, "NULL -> ABSENT");
    ck_eq((int)x86_settings_load(buf, 0), X86_SETTINGS_EABSENT, "len 0 -> ABSENT");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 0, "ABSENT still left defaults");

    /* E. Bad magic. */
    memcpy(bad, buf, (size_t)n);
    bad[0] ^= 0xFF;
    ck_eq((int)x86_settings_load(bad, n), X86_SETTINGS_ENOTSET, "bad magic -> NOTSET");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 0, "NOTSET left defaults");

    /* Too short to be a header at all. */
    ck_eq((int)x86_settings_load(buf, 4), X86_SETTINGS_ENOTSET, "4 bytes -> NOTSET");

    /* G. Unknown version. */
    memcpy(bad, buf, (size_t)n);
    bad[4] = 0x7F;
    ck_eq((int)x86_settings_load(bad, n), X86_SETTINGS_EVERSION, "bad version");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 0, "EVERSION left defaults");

    /* Too many pairs claimed. */
    memcpy(bad, buf, (size_t)n);
    bad[8] = X86_SETTINGS_MAX_PAIRS + 1;
    ck_eq((int)x86_settings_load(bad, n), X86_SETTINGS_ETOOMANY, "too many pairs");

    /* F. TRUNCATED: the count is honest but the bytes stop early. This is the
       half-written-file case and it must not read off the end of the buffer. */
    ck_eq((int)x86_settings_load(buf, n - 1), X86_SETTINGS_ESHORT, "truncated by 1");
    ck_eq((int)x86_settings_load(buf, X86_SETTINGS_HDR_BYTES + 4), X86_SETTINGS_ESHORT,
          "header-sized buffer with a non-zero count");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 0, "ESHORT left defaults");

    /* E. Bad CRC -- EVERY byte the CRC alone protects, one at a time. A CRC
       check that only ever sees one mutation is a CRC check that might be
       comparing the wrong range, and this is what pins the range: from the
       reserved word (offset 12) through the last pair byte.

       The magic, version and count words below offset 12 are deliberately NOT
       in this loop: each has its own earlier, more specific check, so a flipped
       bit in one of them is caught as NOTSET / EVERSION / TOOMANY. Asserting
       ECRC there would be asserting that the cheaper check does not run. */
    for (unsigned long i = 12; i < n - 4; i++) {
        memcpy(bad, buf, (size_t)n);
        bad[i] ^= 0x01;
        ck_eq((int)x86_settings_load(bad, n), X86_SETTINGS_ECRC, "flipped bit -> ECRC");
    }
    ck(n - 4 > 12, "the CRC-only range is not empty");

    /* And the CRC word itself. */
    memcpy(bad, buf, (size_t)n);
    bad[n - 1] ^= 0x80;
    ck_eq((int)x86_settings_load(bad, n), X86_SETTINGS_ECRC, "corrupt CRC word");

    /* An out-of-range VALUE in an otherwise valid file: reported, that setting
       keeps its default, and the file is not thrown away wholesale. */
    {
        unsigned char h[X86_SETTINGS_MAX_BYTES];
        memset(h, 0, sizeof h);
        /* magic, version, count 2 */
        memcpy(h, buf, X86_SETTINGS_HDR_BYTES);
        h[8] = 2; h[9] = h[10] = h[11] = 0;
        /* pair 0: cpu_profile = 99 (out of range) */
        h[16] = X86_SET_CPU_PROFILE; h[20] = 99;
        /* pair 1: use_cmdtail = 1 (fine) */
        h[24] = X86_SET_USE_CMDTAIL; h[28] = 1;
        {
            unsigned long len = X86_SETTINGS_HDR_BYTES + 2 * X86_SETTINGS_PAIR_BYTES;
            unsigned long c = x86_settings_crc32(h, len);
            h[len + 0] = (unsigned char)(c & 0xFF);
            h[len + 1] = (unsigned char)((c >> 8) & 0xFF);
            h[len + 2] = (unsigned char)((c >> 16) & 0xFF);
            h[len + 3] = (unsigned char)((c >> 24) & 0xFF);
            ck_eq((int)x86_settings_load(h, len + 4), X86_SETTINGS_OK,
                  "one bad value is still an OK file");
            ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 0,
                  "the bad value kept its default, NOT a clamp");
            ck_eq(x86_settings_get(X86_SET_USE_CMDTAIL), 1,
                  "the good value beside it still applied");
        }
    }
}

/* ---- H. forward compatibility -------------------------------------------- */

static void test_unknown_pair(void)
{
    unsigned char h[X86_SETTINGS_MAX_BYTES];
    unsigned char out[X86_SETTINGS_MAX_BYTES];
    unsigned long len, c, n;

    x86_settings_base_clear();
    x86_settings_reset();

    memset(h, 0, sizeof h);
    h[0] = 'X'; h[1] = 'S'; h[2] = 'E'; h[3] = 'T';
    h[4] = 1;
    h[8] = 2;
    /* pair 0: a setting from a FUTURE firmware */
    h[16] = 200; h[20] = 42;
    /* pair 1: one we do have */
    h[24] = X86_SET_CPU_PROFILE; h[28] = 3;

    len = X86_SETTINGS_HDR_BYTES + 2 * X86_SETTINGS_PAIR_BYTES;
    c = x86_settings_crc32(h, len);
    h[len + 0] = (unsigned char)(c & 0xFF);
    h[len + 1] = (unsigned char)((c >> 8) & 0xFF);
    h[len + 2] = (unsigned char)((c >> 16) & 0xFF);
    h[len + 3] = (unsigned char)((c >> 24) & 0xFF);

    ck_eq((int)x86_settings_load(h, len + 4), X86_SETTINGS_OK, "unknown id is not fatal");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 3, "the known pair applied");

    /* THE PART THAT MATTERS: the unknown pair is CARRIED into the next save, so
       downgrade-then-upgrade does not silently lose the user's choices. */
    x86_settings_set(X86_SET_USE_CMDTAIL, 1);
    n = x86_settings_save(out, sizeof out);
    ck_eq((int)out[8], 3, "save carried the unknown pair (2 known + 1 unknown)");
    {
        int seen = 0;
        for (unsigned long i = 0; i < (n - 4 - X86_SETTINGS_HDR_BYTES)
                                    / X86_SETTINGS_PAIR_BYTES; i++)
            if (out[X86_SETTINGS_HDR_BYTES + i * X86_SETTINGS_PAIR_BYTES] == 200
                && out[X86_SETTINGS_HDR_BYTES + i * X86_SETTINGS_PAIR_BYTES + 4] == 42)
                seen = 1;
        ck(seen, "the unknown pair survived with its value");
    }
}

/* ---- I. the .cfg text layer ---------------------------------------------- */

static void test_cfg_text(void)
{
    x86_cfg_result_t r;

    /* No file. */
    ck_eq((int)x86_cfg_parse(0, 0, &r), X86_CFG_EABSENT, "no .cfg -> ABSENT");

    /* A DOSBOX-SHAPED FILE. Sections, DOSBox's own key names and units, '#' and
       ';' comments, a trailing comment, ':' as a separator, CRLF, case
       insensitivity, quoting, and blank lines. */
    ck_eq((int)cfg("# a comment\r\n"
                   "; another\r\n"
                   "\r\n"
                   "[CPU]\r\n"
                   "  Cycles = 6000   # trailing comment\r\n"
                   "\r\n"
                   "[dos]\r\n"
                   "xms    = true\r\n"
                   "ems_kb = \"128\"\r\n"
                   "use_cmdtail: ON\r\n", &r), X86_CFG_OK, "a dosbox-shaped .cfg parses");
    ck_eq((int)r.applied, 4, "four keys applied");
    ck_eq((int)r.unknown, 0, "no unknown keys");
    ck_eq((int)r.bad_value, 0, "no bad values");
    ck_eq((int)r.malformed, 0, "no malformed lines");
    ck_eq((int)r.commands, 0, "no autoexec commands");
    ck_eq(x86_settings_default(X86_SET_CPU_CYCLES), 6000, "cycles became the default");
    ck_eq(x86_settings_default(X86_SET_XMS_KB), 4096, "xms = true is a real amount");
    ck_eq(x86_settings_default(X86_SET_EMS_KB), 128, "quoted value parsed");
    ck_eq(x86_settings_default(X86_SET_USE_CMDTAIL), 1, "use_cmdtail became the default");

    /* `cycles = max` is DOSBox's word for the fastest rung, so it lands on the
       PROFILE and not on cycles -- "as fast as it goes" is a rung, not a rate. */
    ck_eq((int)cfg("[cpu]\ncycles = max\n", &r), X86_CFG_OK, "cycles = max parses");
    ck_eq((int)r.applied, 1, "cycles = max applied");
    ck_eq(x86_settings_base_is_set(X86_SET_CPU_PROFILE), 1, "and set the PROFILE");
    ck_eq(x86_settings_default(X86_SET_CPU_PROFILE), 0, "to rung 0, 'Max'");
    ck_eq(x86_settings_base_is_set(X86_SET_CPU_CYCLES), 0, "and NOT cycles");
    ck_eq((int)cfg("[cpu]\ncycles = AUTO\n", &r), X86_CFG_OK, "cycles = auto too");
    ck_eq(x86_settings_base_is_set(X86_SET_CPU_PROFILE), 1, "auto sets the profile");

    /* `xms = false` is NOT the same as saying nothing. This is the whole reason
       "unspecified" is base_is_set() and not the value 0. */
    ck_eq((int)cfg("[dos]\nxms = false\n", &r), X86_CFG_OK, "xms = false parses");
    ck_eq(x86_settings_base_is_set(X86_SET_XMS_KB), 1, "and IS specified");
    ck_eq(x86_settings_default(X86_SET_XMS_KB), 0, "as zero");
    ck_eq((int)cfg("[dos]\n", &r), X86_CFG_OK, "an empty section parses");
    ck_eq(x86_settings_base_is_set(X86_SET_XMS_KB), 0, "saying nothing is different");

    /* SECTIONS ARE NOT REQUIRED -- the .cfg files on the card are flat. */
    ck_eq((int)cfg("cycles = 3000\nxms_kb = 512\n", &r), X86_CFG_OK, "a flat file");
    ck_eq((int)r.applied, 2, "both keys applied with no section header");
    /* ...but a key placed in the WRONG named section is not silently taken. */
    ck_eq((int)cfg("[dos]\ncycles = 3000\n", &r), X86_CFG_OK, "wrong section parses");
    ck_eq((int)r.unknown, 1, "and the key is not found there");
    ck_eq(x86_settings_base_is_set(X86_SET_CPU_CYCLES), 0, "so nothing was set");
    /* An UNKNOWN section is not an error, and its keys are still looked up --
       DOSBox has many sections this core will never have. */
    ck_eq((int)cfg("[sdl]\nfullscreen = true\n[render]\nscaler = normal2x\n"
                   "[cpu]\ncycles = 800\n", &r), X86_CFG_OK, "unknown sections");
    ck_eq((int)r.unknown, 2, "their keys are unknown, not fatal");
    ck_eq(x86_settings_default(X86_SET_CPU_CYCLES), 800, "and [cpu] still worked");

    /* base_clear() on EVERY path, so the previous title's .cfg cannot survive. */
    ck_eq((int)cfg("# nothing at all\n", &r), X86_CFG_OK, "comment-only file");
    ck_eq((int)r.applied, 0, "comment-only file applies nothing");
    ck_eq(x86_settings_base_is_set(X86_SET_CPU_CYCLES), 0,
          "the previous title's .cfg is GONE");

    /* Unknown keys -- which is what every .cfg on the card is full of. */
    ck_eq((int)cfg("ss_big_stack = on\nfast_boot = off\ncycles = 2000\n", &r),
          X86_CFG_OK, "unknown keys are not fatal");
    ck_eq((int)r.applied, 1, "the key we do have still applied");
    ck_eq((int)r.unknown, 2, "both foreign keys counted");
    ck_str(r.first_unknown, "ss_big_stack", "the first unknown key is named");
    ck_eq(x86_settings_default(X86_SET_CPU_CYCLES), 2000, "and the good key took");

    /* Bad values: named, not clamped, and the rest of the file still applies. */
    ck_eq((int)cfg("cpu_profile = 99\nuse_cmdtail = maybe\nxms_kb = 512\n", &r),
          X86_CFG_OK, "bad values are not fatal");
    ck_eq((int)r.bad_value, 2, "both bad values counted");
    ck_str(r.first_bad, "cpu_profile", "the first bad key is named");
    ck_eq(x86_settings_base_is_set(X86_SET_CPU_PROFILE), 0, "bad value did not latch");
    ck_eq(x86_settings_default(X86_SET_CPU_PROFILE), 0, "and it kept its default");
    ck_eq(x86_settings_default(X86_SET_XMS_KB), 512, "the good key after them applied");

    /* Malformed lines, including an unterminated section header. */
    ck_eq((int)cfg("this line has no separator\n= novalue\n[unclosed\n"
                   "cycles = 1000\n", &r), X86_CFG_OK, "malformed lines are not fatal");
    ck_eq((int)r.malformed, 3, "all three malformed lines counted");
    ck_eq((int)r.applied, 1, "and the good line still applied");

    /* THE GRANULARITY RULE. EMS pages are 16 KB and all-or-nothing, so 8 and 24
       are REFUSED rather than rounded -- a reservation that cannot hold a whole
       page is a silent discard. XMS has no such rule and 1 KB steps are real. */
    ck_eq((int)cfg("ems_kb = 8\n", &r), X86_CFG_OK, "ems_kb = 8 parses");
    ck_eq((int)r.bad_value, 1, "ems_kb = 8 REFUSED, not rounded");
    ck_eq((int)cfg("ems_kb = 24\n", &r), X86_CFG_OK, "ems_kb = 24 parses");
    ck_eq((int)r.bad_value, 1, "ems_kb = 24 REFUSED");
    ck_eq((int)cfg("ems_kb = 64\n", &r), X86_CFG_OK, "ems_kb = 64 parses");
    ck_eq((int)r.applied, 1, "ems_kb = 64 accepted");
    ck_eq((int)cfg("ems_kb = 0\n", &r), X86_CFG_OK, "ems_kb = 0 parses");
    ck_eq((int)r.applied, 1, "0 stays legal -- it is 'advertise none'");
    ck_eq((int)cfg("xms_kb = 513\n", &r), X86_CFG_OK, "xms_kb = 513 parses");
    ck_eq((int)r.applied, 1, "XMS has no granularity rule");
    ck_eq((int)cfg("ems = true\n", &r), X86_CFG_OK, "ems = true parses");
    ck_eq(x86_settings_default(X86_SET_EMS_KB), 1024, "and is a whole window");

    /* A binary file -- a renamed .dsk, a savestate, a half-written write. */
    {
        const unsigned char blob[] = { 'c', 'p', 'u', '_', 's', 'p', 'e', 'e',
                                       'd', '=', '1', '\n', 0x00, 0x01 };
        ck_eq((int)x86_cfg_parse(blob, sizeof blob, &r), X86_CFG_EBINARY,
              "a NUL makes it BINARY, and nothing is applied");
        ck_eq(x86_settings_default(X86_SET_CPU_PROFILE), 0,
              "BINARY applied nothing at all");
    }

    /* Over the size cap: refused whole, never truncated. */
    {
        static unsigned char big[X86_CFG_MAX_BYTES + 64];
        memset(big, '\n', sizeof big);
        memcpy(big, "cycles = 5000\n", 14);
        ck_eq((int)x86_cfg_parse(big, sizeof big, &r), X86_CFG_ETOOBIG,
              "over the cap -> TOOBIG");
        ck_eq(x86_settings_default(X86_SET_CPU_PROFILE), 0,
              "TOOBIG applied nothing -- not the first N bytes");
        ck_eq((int)x86_cfg_parse(big, X86_CFG_MAX_BYTES, &r), X86_CFG_OK,
              "exactly at the cap is fine");
    }

    /* out may be NULL. */
    ck_eq((int)cfg("cycles = 1000\n", 0), X86_CFG_OK, "NULL result struct is fine");

    /* Enumeration: every published key resolves, and probing through the id
       proves the name -> id wiring rather than just the name. */
    {
        int i;
        for (i = 0; x86_cfg_key_name(i); i++) {
            int id = x86_cfg_key_id(i);
            ck(id >= 0, "every enumerated key has an id");
            if (x86_cfg_key_is_str(i))
                ck(id < X86_STR_COUNT, "string key id is in the string space");
            else
                ck(id < X86_SET_COUNT, "int key id is in the setting space");
        }
        ck(i > 0, "the key table enumerates");
        ck_eq(x86_cfg_key_id(i), -1, "past the end -> -1");
        ck(x86_cfg_key_section(i) == 0, "past the end -> NULL section");
    }
}

/* ---- I2. [autoexec] ------------------------------------------------------ */

static void test_autoexec(void)
{
    x86_cfg_result_t r;

    /* THE SHAPE A USER WILL ACTUALLY PASTE IN, straight out of a desktop
       dosbox.conf. Every line here except the last two is work this core does
       elsewhere, and none of them may be reported as a mistake. */
    ck_eq((int)cfg("[autoexec]\n"
                   "@echo off\n"
                   "mount c \"/home/me/dos\"\n"
                   "c:\n"
                   "cd GAMES\\KEEN4\n"
                   "KEEN4E.EXE /NOJOY /SND\n"
                   "exit\n", &r), X86_CFG_OK, "a pasted [autoexec] parses");
    ck_eq((int)r.unknown, 0, "and names no unknown key");
    ck_eq((int)r.bad_value, 0, "and no bad value");
    ck_eq((int)r.malformed, 0, "and NO malformed line -- these are commands");
    ck_eq((int)r.commands, 1, "exactly one of them is the program");
    ck_str(x86_settings_str(X86_STR_CWD), "GAMES\\KEEN4", "cd became the cwd");
    ck_str(x86_settings_str(X86_STR_ENTRY), "KEEN4E.EXE", "the command became entry");
    ck_str(x86_settings_str(X86_STR_ARGS), "/NOJOY /SND", "and the rest became args");

    /* `cd\PATH` with no space, which is what a real AUTOEXEC.BAT is full of. */
    cfg("[autoexec]\ncd\\WOLF3D\nWOLF3D.EXE\n", &r);
    ck_str(x86_settings_str(X86_STR_CWD), "\\WOLF3D", "cd\\PATH with no space");
    ck_str(x86_settings_str(X86_STR_ENTRY), "WOLF3D.EXE", "and the program after it");

    /* A command with NO arguments still SETS args, to empty. Leaving it unset
       would let the guest's own AUTOEXEC.BAT tail leak in behind a line that
       said, completely, how to launch. */
    cfg("[autoexec]\nCAT.EXE\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "CAT.EXE", "bare command");
    ck_eq(x86_settings_str_is_set(X86_STR_ARGS), 1, "args IS set");
    ck_str(x86_settings_str(X86_STR_ARGS), "", "to empty");

    /* LAST ONE WINS. */
    cfg("[autoexec]\nFIRST.EXE -a\nSECOND.EXE -b\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "SECOND.EXE", "last command wins");
    ck_str(x86_settings_str(X86_STR_ARGS), "-b", "with its own arguments");
    ck_eq((int)r.commands, 2, "and both were counted");

    /* A section with NOTHING this core can run reports zero commands, which is
       the reading that tells a user their launch line was skipped. */
    cfg("[autoexec]\nmount c .\nc:\n", &r);
    ck_eq((int)r.commands, 0, "an all-inert section yields no command");
    ck_eq((int)r.malformed, 0, "and still reports no error");
    ck_eq(x86_settings_str_is_set(X86_STR_ENTRY), 0, "and sets no entry");

    /* `=` and `#` INSIDE a command line are DATA. Applying the key=value rules
       in here would cut a real command line in half. */
    cfg("[autoexec]\nSETUP.EXE /MODE=VGA /X\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "SETUP.EXE", "'=' in a command line");
    ck_str(x86_settings_str(X86_STR_ARGS), "/MODE=VGA /X", "is data, not a key");
    ck_eq((int)r.malformed, 0, "and is not malformed");
    cfg("[autoexec]\nGAME.EXE -w #3\n", &r);
    ck_str(x86_settings_str(X86_STR_ARGS), "-w #3", "'#' in a command line is data");

    /* `cdplayer` is not `cd`. */
    cfg("[autoexec]\nCDPLAYER.EXE\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "CDPLAYER.EXE", "'cd' matches a whole verb");
    ck_eq(x86_settings_str_is_set(X86_STR_CWD), 0, "and did not set a cwd");

    /* Leaving the section returns to key=value. */
    cfg("[autoexec]\nGAME.EXE\n[cpu]\ncycles = 2500\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "GAME.EXE", "the command was taken");
    ck_eq(x86_settings_default(X86_SET_CPU_CYCLES), 2500, "and the next section parsed");

    /* Comments still work inside it -- they are stripped before the section
       test, which is what a user pasting a commented config expects. */
    cfg("[autoexec]\n# GAME.EXE\n; OTHER.EXE\nREAL.EXE\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "REAL.EXE", "commented commands skipped");
    ck_eq((int)r.commands, 1, "and only the real one counted");
}

/* ---- J. the three layers ------------------------------------------------- */

static void test_layers(void)
{
    unsigned char buf[X86_SETTINGS_MAX_BYTES];
    x86_cfg_result_t r;
    unsigned long n;

    /* Layer 1 alone. */
    x86_settings_base_clear();
    x86_settings_reset();
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 0, "layer 1: the firmware default");

    /* Layer 2: the .cfg REPLACES the default. */
    cfg("cpu_profile = 6\n", &r);
    x86_settings_reset();
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 6, "layer 2: the .cfg is the default");
    ck_eq(x86_settings_dirty(), 0, "a .cfg value is not a change to be saved");

    /* AND IT IS NOT WRITTEN TO THE .dosset. If it were, editing the .cfg later
       would silently do nothing for every title ever launched. */
    n = x86_settings_save(buf, sizeof buf);
    ck_eq((int)n, X86_SETTINGS_HDR_BYTES + 4,
          "a value the .cfg already states is NOT recorded in the .dosset");

    /* Layer 3: the user's own choice wins over the .cfg. */
    x86_settings_set(X86_SET_CPU_PROFILE, 2);
    ck_eq(x86_settings_dirty(), 1, "the menu row differs from the .cfg -> dirty");
    n = x86_settings_save(buf, sizeof buf);
    ck_eq((int)buf[8], 1, "and only that one pair is written");

    /* Reload with the .cfg still in force: the user's value wins. */
    ck_eq((int)x86_settings_load(buf, n), X86_SETTINGS_OK, "reload with a .cfg");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 2, "layer 3 beats layer 2");

    /* Now EDIT THE .cfg. The user's explicit override still wins -- that is the
       point of it being an override. */
    cfg("cpu_profile = 9\n", &r);
    ck_eq((int)x86_settings_load(buf, n), X86_SETTINGS_OK, "reload after a .cfg edit");
    ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 2, "an overridden title does not move");

    /* But a title that NEVER overrode by hand DOES move when the .cfg changes.
       This is the property the whole "the .cfg is a default" design exists for. */
    {
        unsigned char empty[X86_SETTINGS_MAX_BYTES];
        unsigned long en;
        x86_settings_base_clear();
        x86_settings_reset();
        en = x86_settings_save(empty, sizeof empty);   /* a never-touched title */

        cfg("cpu_profile = 9\n", &r);
        x86_settings_load(empty, en);
        ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 9, "an untouched title MOVES");

        cfg("cpu_profile = 3\n", &r);
        x86_settings_load(empty, en);
        ck_eq(x86_settings_get(X86_SET_CPU_PROFILE), 3, "and moves again");
    }

    /* load() resets first, so a second title cannot inherit the first's answers. */
    x86_settings_base_clear();
    x86_settings_set(X86_SET_XMS_KB, 4096);
    x86_settings_load(0, 0);
    ck_eq(x86_settings_get(X86_SET_XMS_KB), 0,
          "load() of an absent file drops the previous title's answers");
}

/* ---- K. strings ---------------------------------------------------------- */

static void test_strings(void)
{
    x86_cfg_result_t r;

    x86_settings_base_clear();
    ck_str(x86_settings_str(X86_STR_ENTRY), "", "unset string is empty, never NULL");
    ck_eq(x86_settings_str_is_set(X86_STR_ENTRY), 0, "unset string is not set");
    ck_str(x86_settings_str((x86_setting_str_id_t)X86_STR_COUNT), "",
           "past the end is empty, not a fault");

    /* THE LAUNCH PARAMETERS -- the owner's headline ask. */
    ck_eq((int)cfg("entry = KEEN4E.EXE\nargs = /NOJOY /SND\n", &r), X86_CFG_OK,
          "entry and args parse");
    ck_eq((int)r.applied, 2, "both launch parameters applied");
    ck_str(x86_settings_str(X86_STR_ENTRY), "KEEN4E.EXE", "entry round-tripped");
    ck_str(x86_settings_str(X86_STR_ARGS), "/NOJOY /SND", "args kept its spaces");
    ck_eq(x86_settings_str_is_set(X86_STR_ARGS), 1, "args is set");

    /* Case is preserved in a VALUE even though the KEY is case-insensitive. */
    cfg("ENTRY = MixedCase.Exe\n", &r);
    ck_str(x86_settings_str(X86_STR_ENTRY), "MixedCase.Exe", "value case preserved");

    /* An empty value is legal and DIFFERENT from saying nothing: `args =` means
       "run it with no arguments". */
    cfg("args =\n", &r);
    ck_str(x86_settings_str(X86_STR_ARGS), "", "empty args is empty");
    ck_eq(x86_settings_str_is_set(X86_STR_ARGS), 1, "but it IS set");
    ck_eq(x86_settings_str_is_set(X86_STR_ENTRY), 0, "while entry is not");

    /* Quoting is how leading/trailing space survives the trim. */
    cfg("args = \" -l \"\n", &r);
    ck_str(x86_settings_str(X86_STR_ARGS), " -l ", "quotes preserve edge spaces");

    /* A trailing comment still needs whitespace before the marker, so a '#'
       inside a command tail is not a data-loss bug. */
    cfg("args = a#b\n", &r);
    ck_str(x86_settings_str(X86_STR_ARGS), "a#b", "'#' with no space is data");
    cfg("args = a #b\n", &r);
    ck_str(x86_settings_str(X86_STR_ARGS), "a", "'#' after a space is a comment");

    /* REFUSED, NOT TRUNCATED. Half a command line is a different command line. */
    {
        char line[X86_SETTINGS_STR_MAX + 32];
        int i;
        strcpy(line, "args = ");
        for (i = 0; i < X86_SETTINGS_STR_MAX; i++)
            line[7 + i] = 'x';
        line[7 + X86_SETTINGS_STR_MAX] = '\n';
        line[8 + X86_SETTINGS_STR_MAX] = 0;

        x86_settings_base_clear();
        ck_eq((int)cfg(line, &r), X86_CFG_OK, "an over-long value parses");
        ck_eq((int)r.bad_value, 1, "and is counted BAD");
        ck_str(r.first_bad, "args", "and named");
        ck_eq(x86_settings_str_is_set(X86_STR_ARGS), 0,
              "an over-long value is REFUSED, not truncated");
        ck_str(x86_settings_str(X86_STR_ARGS), "", "nothing was stored");
    }

    /* The longest value that DOES fit still fits -- an off-by-one here would
       make the check above pass for the wrong reason. */
    {
        char line[X86_SETTINGS_STR_MAX + 32];
        int i;
        strcpy(line, "args = ");
        for (i = 0; i < X86_SETTINGS_STR_MAX - 1; i++)
            line[7 + i] = 'y';
        line[7 + X86_SETTINGS_STR_MAX - 1] = '\n';
        line[8 + X86_SETTINGS_STR_MAX - 1] = 0;

        x86_settings_base_clear();
        cfg(line, &r);
        ck_eq((int)r.applied, 1, "the longest value that fits is accepted");
        ck_eq((int)strlen(x86_settings_str(X86_STR_ARGS)), X86_SETTINGS_STR_MAX - 1,
              "and is stored whole");
    }

    /* base_clear() drops strings too, so the previous title's entry cannot
       launch this one's disk. */
    cfg("entry = A.EXE\n", &r);
    x86_settings_base_clear();
    ck_str(x86_settings_str(X86_STR_ENTRY), "",
           "base_clear() drops the previous title's entry");
    ck_eq(x86_settings_str_is_set(X86_STR_ENTRY), 0, "and its is_set");

    /* Strings are .cfg-only and have NO wire representation. Saving after
       setting one writes nothing, because there is no menu row that could have
       changed it. */
    {
        unsigned char buf[X86_SETTINGS_MAX_BYTES];
        cfg("entry = B.EXE\n", &r);
        x86_settings_reset();
        ck_eq((int)x86_settings_save(buf, sizeof buf), X86_SETTINGS_HDR_BYTES + 4,
              "a string does not make the .dosset non-empty");
    }
}

/* ---- L. the .cfg files that are actually on the card --------------------- */

static void test_shipped_cfg(void)
{
    x86_cfg_result_t r;

    /* This is the header every shipped .cfg carries, verbatim from
       references/game-and-watch-retro-go-sd/roms/dos/WOLF3D.cfg, plus the keys
       it names -- ALL COMMENTED OUT, which is how the packer generates them.
       The claim being graded is the one this change most needs to be true:
       ADDING THIS FEATURE MOVES NOTHING, because as shipped these files apply
       nothing at all. */
    static const char shipped[] =
        "# WOLF3D.cfg -- per-game settings. GENERATED ONCE and NEVER overwritten;\n"
        "# delete it for a fresh copy. Spec, every key, and the evidence behind each:\n"
        "#\n"
        "# `key = value`, one per line. `#`/`;` comment. Case-insensitive. LF or CRLF.\n"
        "#\n"
        "#   compiled-in default  <  THIS FILE  <  the device's options menu\n"
        "#\n"
        "# EVERY LINE BELOW IS A COMMENT: as shipped this file changes NOTHING.\n"
        "\n"
        "#ss_big_stack = on\n"
        "\n"
        "#fast_boot = off\n"
        "\n"
        "#ems = on\n"
        "\n"
        "#ems_kb = 64\n";

    x86_settings_base_clear();
    ck_eq((int)cfg(shipped, &r), X86_CFG_OK, "a shipped .cfg parses");
    ck_eq((int)r.applied, 0, "a shipped .cfg applies NOTHING");
    ck_eq((int)r.unknown, 0, "and names no unknown key");
    ck_eq((int)r.bad_value, 0, "and no bad value");
    ck_eq((int)r.malformed, 0, "and no malformed line");
    x86_settings_reset();
    for (int i = 0; i < X86_SET_COUNT; i++)
        ck_eq(x86_settings_get((x86_setting_id_t)i),
              x86_settings_builtin((x86_setting_id_t)i),
              "a shipped .cfg leaves every setting exactly as it was");

    /* And if a user UNCOMMENTS one of the prior port's keys, they get one line
       naming it rather than a silent no-op or a refused launch. */
    ck_eq((int)cfg("ss_big_stack = on\n", &r), X86_CFG_OK, "a foreign key parses");
    ck_eq((int)r.unknown, 1, "and is counted");
    ck_str(r.first_unknown, "ss_big_stack", "and named");
}

/* ---- CRC32 ---------------------------------------------------------------- */

static void test_crc(void)
{
    /* The standard reflected CRC-32 of "123456789". Pins the polynomial and the
       reflection so a rewrite cannot quietly change the on-card format. */
    ck_eq((int)(x86_settings_crc32("123456789", 9) == 0xCBF43926UL), 1,
          "crc32(\"123456789\") == 0xCBF43926");
    ck_eq((int)(x86_settings_crc32("", 0) == 0UL), 1, "crc32 of nothing is 0");
}

int main(void)
{
    test_defaults();
    test_ranges();
    test_roundtrip();
    test_corrupt();
    test_unknown_pair();
    test_cfg_text();
    test_autoexec();
    test_layers();
    test_strings();
    test_shipped_cfg();
    test_crc();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
