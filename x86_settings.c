/*
 * x86_settings.c -- see x86_settings.h for the contract, the three layers and
 * why this file does no I/O at all.
 *
 * Freestanding C89-ish. Compiles for the host harness (tests/host_settings.c)
 * and for cortex-m7 from the same source. No malloc, no float, no file I/O --
 * the caller hands over bytes and takes bytes back, which is what keeps the
 * MAX_OPEN_FILES = 8 discipline in one small place in src/main.c instead of
 * spread through here.
 *
 * COLD PATH ONLY. Once at launch, once per menu change. Nothing in this file
 * may be reached from cpu_exec*.S or from the frame loop.
 */

#include <stdio.h>
#include <string.h>
#include "x86_settings.h"

/*
 * ---- NEGATIVE CONTROLS ------------------------------------------------------
 *
 * Each of these DISABLES one of this file's guards and every one of them is 0
 * in every real build. They exist because a test that only ever sees the guard
 * working cannot tell a guard from a coincidence: tests/test_settings.py
 * --negative rebuilds with each in turn and REQUIRES the corresponding check to
 * fail. A control that does not fail is a check that was not testing anything.
 *
 * Same idiom, and same reason, as -DX86_SEAM_FIX=0 in cpu_exec.S's harness.
 */
#ifndef X86_SETTINGS_CTL_NOCRC
#define X86_SETTINGS_CTL_NOCRC 0      /* 1: do not verify the trailing CRC32 */
#endif
#ifndef X86_SETTINGS_CTL_NORANGE
#define X86_SETTINGS_CTL_NORANGE 0    /* 1: store pair values without a range
                                         check, i.e. trust the file           */
#endif
#ifndef X86_SETTINGS_CTL_NOVERSION
#define X86_SETTINGS_CTL_NOVERSION 0  /* 1: accept any version word           */
#endif
#ifndef X86_SETTINGS_CTL_TRUNCSTR
#define X86_SETTINGS_CTL_TRUNCSTR 0   /* 1: truncate an over-long string
                                         instead of refusing it              */
#endif

/*
 * ---- The table -------------------------------------------------------------
 *
 * One row per setting, in enum order. `def` is what a title gets when nobody
 * has said otherwise; [lo, hi] is what this firmware will accept from a file.
 *
 * THE RANGE IS NOT DECORATION. A value outside it is a file written by a
 * different firmware, a truncated write, or a bug, and the only safe reading of
 * any of those is "this setting was never configured". It is reported by name
 * rather than clamped: clamping invents a value nobody chose and then persists
 * it, so the user is left with a number they cannot account for and no record
 * of where it came from.
 */
typedef struct {
    const char *name;
    int def;
    int lo;
    int hi;
} x86_setting_desc_t;

static const x86_setting_desc_t x86_setting_desc[X86_SET_COUNT] = {
    /* cpu_profile. Default 0 = "Max", which is what cpu_profile_init() asked
       the app-scoped store for as ITS default, so a title with no files runs
       unthrottled exactly as it does today. Upper bound 15 is a sanity bound
       and NOT CPU_PROFILE_COUNT-1 -- see the header: a copy of another file's
       table length here is a second thing to keep in step, and the consumer
       validates against its own table anyway. */
    { "cpu_profile",  0, 0, 15 },

    /* use_cmdtail. Default 0 = today's guest_use_cmdtail initialiser. */
    { "use_cmdtail",  0, 0, 1 },

    /* xms_kb. Value 0 means "advertise none"; "unspecified" is base_is_set()
       answering 0 (see the header). The ceiling is 8192 because the XMS store
       lives in the pagefile above every pager slot and a larger claim is a
       packaging mistake rather than a request; the consumer still has the
       pagefile's own veto, which is what turns "no pagefile" into an honest
       zero whatever a .cfg asked for. */
    { "xms_kb",       0, 0, 8192 },

    /* ems_kb. 1024 KB is X86_EMS_PAGES * 16, i.e. the whole of the aux window
       the pager was given at reset -- asking for more than exists could only be
       honoured by silently giving less, which is the shape of failure this
       module refuses everywhere else. The 16 KB granularity is enforced by
       x86_cfg.c's value parser and not by this range, because 0 has to stay
       legal (it is "advertise none") while 8 and 24 must not. */
    { "ems_kb",       0, 0, 1024 },

    /* cpu_cycles, in kips == DOSBox cycles. The ceiling is far above anything
       the silicon can serve (~3,800 kips overclocked) on purpose: a .cfg
       carrying a number copied out of somebody's dosbox.conf should resolve to
       the fastest rung rather than be refused as a bad value, and the consumer
       clamps to its own table's top when it picks the nearest rung. */
    { "cpu_cycles",   0, 0, 200000 },
};

/* String rows. Separate from the table above because they are .cfg-only and
   have no wire representation at all (x86_settings.h says why). */
static const char *const x86_setting_str_name_tbl[X86_STR_COUNT] = {
    "entry", "args", "cwd",
};

/*
 * The live table, the table as it was when load() finished, and the pairs
 * load() did not recognise.
 *
 * WHY KEEP `loaded`: dirty() is a memcmp against it rather than a flag that
 * set() raises. A user who presses right and then left again has changed
 * nothing, and a flag would still write the file -- creating a .dosset for a
 * title whose settings are all default, which is exactly the litter this is
 * supposed to avoid.
 */
static int x86_set_cur[X86_SET_COUNT];
/* Declared here, documented at x86_settings_get(): the live table is zero until
   something resets it, and zero is a legal value for every setting. */
static int x86_set_inited;
static int x86_set_loaded[X86_SET_COUNT];

typedef struct { unsigned long id; int val; } x86_set_pair_t;
static x86_set_pair_t x86_set_unknown[X86_SETTINGS_MAX_PAIRS];
static int x86_set_unknown_n;

/*
 * The base layer (the .cfg). Two parallel arrays rather than a sentinel value:
 * every setting's range includes every value a sentinel could have used, and
 * "the base happens to equal the firmware default" has to stay distinguishable
 * from "there is no base" -- x86_settings_base_is_set() is what a consumer asks
 * to tell "unspecified" from "specified as 0", which is precisely the
 * distinction xms_kb and ems_kb are built on.
 */
static int x86_set_base[X86_SET_COUNT];
static unsigned char x86_set_base_on[X86_SET_COUNT];

static char x86_set_str[X86_STR_COUNT][X86_SETTINGS_STR_MAX];
static unsigned char x86_set_str_on[X86_STR_COUNT];

const char *x86_settings_status_name(x86_settings_status_t s)
{
    switch (s) {
    case X86_SETTINGS_OK:       return "OK";
    case X86_SETTINGS_EABSENT:  return "ABSENT";
    case X86_SETTINGS_ENOTSET:  return "NOTSET";
    case X86_SETTINGS_EVERSION: return "VERSION";
    case X86_SETTINGS_ETOOMANY: return "TOOMANY";
    case X86_SETTINGS_ESHORT:   return "SHORT";
    case X86_SETTINGS_ECRC:     return "CRC";
    }
    return "?";
}

const char *x86_settings_name(x86_setting_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return "?";
    return x86_setting_desc[id].name;
}

const char *x86_settings_str_name(x86_setting_str_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_STR_COUNT)
        return "?";
    return x86_setting_str_name_tbl[id];
}

/* ---- The base layer ------------------------------------------------------ */

void x86_settings_base_clear(void)
{
    int i;

    for (i = 0; i < X86_SET_COUNT; i++) {
        x86_set_base[i] = 0;
        x86_set_base_on[i] = 0;
    }
    for (i = 0; i < X86_STR_COUNT; i++) {
        x86_set_str[i][0] = 0;
        x86_set_str_on[i] = 0;
    }
}

int x86_settings_base_set(x86_setting_id_t id, int value)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return 0;
    if (value < x86_setting_desc[id].lo || value > x86_setting_desc[id].hi)
        return 0;
    x86_set_base[id] = value;
    x86_set_base_on[id] = 1;
    return 1;
}

int x86_settings_base_is_set(x86_setting_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return 0;
    return x86_set_base_on[id] ? 1 : 0;
}

int x86_settings_builtin(x86_setting_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return 0;
    return x86_setting_desc[id].def;
}

int x86_settings_range(x86_setting_id_t id, int *lo, int *hi)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return 0;
    if (lo) *lo = x86_setting_desc[id].lo;
    if (hi) *hi = x86_setting_desc[id].hi;
    return 1;
}

/* THE EFFECTIVE default: base first, firmware second. Every caller -- reset(),
   save(), the out-of-range report, the host harness -- goes through this, which
   is what makes the .cfg a default rather than a fourth kind of value with its
   own rules. */
int x86_settings_default(x86_setting_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return 0;
    if (x86_set_base_on[id])
        return x86_set_base[id];
    return x86_setting_desc[id].def;
}

/* ---- Strings ------------------------------------------------------------- */

const char *x86_settings_str(x86_setting_str_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_STR_COUNT)
        return "";
    return x86_set_str[id];
}

int x86_settings_str_is_set(x86_setting_str_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_STR_COUNT)
        return 0;
    return x86_set_str_on[id] ? 1 : 0;
}

int x86_settings_str_set(x86_setting_str_id_t id, const char *s, unsigned long len)
{
    if ((int)id < 0 || (int)id >= X86_STR_COUNT || !s)
        return 0;
    /* REFUSED, NOT TRUNCATED. Half a command line is a different command line,
       and the caller reports the refusal by name -- so the user sees "args" in
       the log instead of watching the guest launch with arguments nobody
       wrote. */
#if X86_SETTINGS_CTL_TRUNCSTR
    if (len >= X86_SETTINGS_STR_MAX)
        len = X86_SETTINGS_STR_MAX - 1;
#else
    if (len >= X86_SETTINGS_STR_MAX)
        return 0;
#endif
    if (len)
        memcpy(x86_set_str[id], s, (size_t)len);
    x86_set_str[id][len] = 0;
    x86_set_str_on[id] = 1;
    return 1;
}

/* ---- CRC and byte order -------------------------------------------------- */

unsigned long x86_settings_crc32(const void *buf, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long crc = 0xFFFFFFFFUL;
    unsigned long i;
    int k;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (unsigned long)(-(long)(crc & 1)));
    }
    return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ---- The live table ------------------------------------------------------ */

void x86_settings_reset(void)
{
    int i;

    /* THE EFFECTIVE default, not the compiled one: this is where a .cfg takes
       effect. base_clear() is NOT called from here on purpose -- the base is
       set before the .dosset is read and has to survive the reset that reading
       it performs. */
    for (i = 0; i < X86_SET_COUNT; i++)
        x86_set_cur[i] = x86_set_loaded[i] = x86_settings_default((x86_setting_id_t)i);
    x86_set_unknown_n = 0;
    x86_set_inited = 1;
}

/*
 * One line, on every failure, naming the failure. See the header: the anti-goal
 * is a settings layer that answers defaults and looks identical to one that
 * works.
 *
 * NOT RATE-LIMITED and it does not need to be: load() runs once per title
 * launch, on the cold path. A once-per-launch line is affordable; a per-frame
 * one would erase the boot output somebody is trying to read.
 */
static x86_settings_status_t fail(x86_settings_status_t s)
{
    printf("DOS: .dosset %s -- defaults\n",
           x86_settings_status_name(s));
    return s;
}

x86_settings_status_t x86_settings_load(const unsigned char *buf, unsigned long len)
{
    unsigned long count, need, i, crc;

    /* FIRST, unconditionally: whatever happens below, the table is defaults
       plus whatever we managed to understand. There is no path out of this
       function that leaves a caller holding another title's settings. */
    x86_settings_reset();

    /* No file. Not an event, not a line of log: this is every title nobody has
       configured, which is all of them until somebody presses right. */
    if (!buf || len == 0)
        return X86_SETTINGS_EABSENT;

    if (len < X86_SETTINGS_HDR_BYTES + 4 || rd32(buf) != X86_SETTINGS_MAGIC)
        return fail(X86_SETTINGS_ENOTSET);
#if !X86_SETTINGS_CTL_NOVERSION
    if (rd32(buf + 4) != X86_SETTINGS_VERSION)
        return fail(X86_SETTINGS_EVERSION);
#endif

    count = rd32(buf + 8);
    if (count > X86_SETTINGS_MAX_PAIRS)
        return fail(X86_SETTINGS_ETOOMANY);

    need = X86_SETTINGS_HDR_BYTES + count * X86_SETTINGS_PAIR_BYTES + 4;
    /* `len` may legitimately be LARGER than `need` -- FatFs hands back whole
       bytes read and the caller over-reads into a fixed buffer -- so this is a
       SHORT check, not an equality check. The CRC covers exactly `need - 4`
       bytes, which is what pins the length that actually mattered. */
    if (len < need)
        return fail(X86_SETTINGS_ESHORT);

    crc = rd32(buf + need - 4);
#if !X86_SETTINGS_CTL_NOCRC
    if (crc != x86_settings_crc32(buf, need - 4))
        return fail(X86_SETTINGS_ECRC);
#else
    (void)crc;
#endif

    for (i = 0; i < count; i++) {
        const unsigned char *p = buf + X86_SETTINGS_HDR_BYTES
                               + i * X86_SETTINGS_PAIR_BYTES;
        unsigned long id = rd32(p);
        /* Through unsigned int first: rd32 returns an unsigned long that is 64
           bits on the host and 32 on the target, and going straight to int
           would mean two different answers for a negative value on the two
           builds this file is compiled for. */
        int v = (int)(unsigned int)rd32(p + 4);

        if (id >= (unsigned long)X86_SET_COUNT) {
            /* A setting this firmware does not have. Carried, not dropped:
               dropping it would mean a downgrade-then-upgrade silently loses
               the user's choices, and the pair costs eight bytes. */
            if (x86_set_unknown_n < X86_SETTINGS_MAX_PAIRS) {
                x86_set_unknown[x86_set_unknown_n].id = id;
                x86_set_unknown[x86_set_unknown_n].val = v;
                x86_set_unknown_n++;
            }
            continue;
        }
        if (!X86_SETTINGS_CTL_NORANGE
            && (v < x86_setting_desc[id].lo || v > x86_setting_desc[id].hi)) {
            /* Reported by name and by value, and NOT clamped. The rest of the
               file is still honoured: one bad field is not evidence against
               the others, and throwing the lot away would turn a one-setting
               bug into a total loss. */
            printf("DOS: .dosset %s=%d out of range [%d,%d] -- default %d\n",
                   x86_setting_desc[id].name, v,
                   x86_setting_desc[id].lo, x86_setting_desc[id].hi,
                   x86_settings_default((x86_setting_id_t)id));
            continue;
        }
        x86_set_cur[id] = x86_set_loaded[id] = v;
    }
    return X86_SETTINGS_OK;
}

unsigned long x86_settings_save(unsigned char *buf, unsigned long cap)
{
    unsigned long n = 0;
    int i;

    if (!buf || cap < X86_SETTINGS_MAX_BYTES)
        return 0;

    wr32(buf + 0, X86_SETTINGS_MAGIC);
    wr32(buf + 4, X86_SETTINGS_VERSION);
    wr32(buf + 12, 0);

    /* Non-default settings first, then the pairs we did not understand. Order
       within the file is not significant to the reader; it is fixed here only
       so that saving the same state twice produces the same bytes, which is
       what lets a test compare files rather than parse them. */
    for (i = 0; i < X86_SET_COUNT && n < X86_SETTINGS_MAX_PAIRS; i++) {
        unsigned char *p;

        /* THE EFFECTIVE default, so a value the .cfg already states is not
           copied into the .dosset. If it were, editing the .cfg afterwards
           would silently do nothing for every title that had ever been
           launched. */
        if (x86_set_cur[i] == x86_settings_default((x86_setting_id_t)i))
            continue;   /* defaults are not written, on purpose */
        p = buf + X86_SETTINGS_HDR_BYTES + n * X86_SETTINGS_PAIR_BYTES;
        wr32(p, (unsigned long)i);
        wr32(p + 4, (unsigned long)(long)x86_set_cur[i]);
        n++;
    }
    for (i = 0; i < x86_set_unknown_n && n < X86_SETTINGS_MAX_PAIRS; i++) {
        unsigned char *p = buf + X86_SETTINGS_HDR_BYTES
                         + n * X86_SETTINGS_PAIR_BYTES;
        wr32(p, x86_set_unknown[i].id);
        wr32(p + 4, (unsigned long)(long)x86_set_unknown[i].val);
        n++;
    }

    wr32(buf + 8, n);
    {
        unsigned long len = X86_SETTINGS_HDR_BYTES + n * X86_SETTINGS_PAIR_BYTES;
        wr32(buf + len, x86_settings_crc32(buf, len));
        return len + 4;
    }
}

int x86_settings_dirty(void)
{
    int i;

    for (i = 0; i < X86_SET_COUNT; i++)
        if (x86_set_cur[i] != x86_set_loaded[i])
            return 1;
    return 0;
}

/*
 * THE LIVE TABLE IS ZERO UNTIL SOMETHING RESETS IT, AND ZERO IS LEGAL FOR EVERY
 * SETTING. That combination is the reason for the lazy reset below, and the
 * prior port did not find it hypothetically: its environment composer generated
 * a CONFIG.SYS reading every default as 0 -- silently replaced by the C static
 * initialiser -- because it asked before anything had called reset().
 *
 * There is no sentinel that could have caught it: every setting's legal range
 * includes 0 by construction (0 means "unspecified" for two of them), so a
 * range check cannot tell "never initialised" from "deliberately zero". A flag
 * on the TABLE can, and this is it.
 *
 * Lazily resetting is exactly what a correct caller does and nothing more:
 * reset() seeds from the EFFECTIVE default, so a .cfg parsed beforehand is
 * still honoured, and a later reset() or load() overwrites this wholesale.
 */
int x86_settings_get(x86_setting_id_t id)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return 0;
    if (!x86_set_inited)
        x86_settings_reset();
    return x86_set_cur[id];
}

void x86_settings_set(x86_setting_id_t id, int value)
{
    if ((int)id < 0 || (int)id >= X86_SET_COUNT)
        return;
    if (!x86_set_inited)
        x86_settings_reset();
    /* A caller handing over something outside the range is a bug in the caller,
       not a corrupt file, so it gets its own line and is refused rather than
       stored -- storing it would put a value in the file that load() would then
       reject, which is a round trip that does not round trip. */
    if (value < x86_setting_desc[id].lo || value > x86_setting_desc[id].hi) {
        printf("DOS: set %s=%d out of range [%d,%d] -- ignored\n",
               x86_setting_desc[id].name, value,
               x86_setting_desc[id].lo, x86_setting_desc[id].hi);
        return;
    }
    x86_set_cur[id] = value;
}
