/*
 * x86_settings.h -- PER-TITLE SETTINGS: the FORMAT and the POLICY.
 *
 * Brought over from the prior DOS port (references/8086tiny/dos_settings.{c,h}),
 * with the setting list replaced by what THIS core can actually honour. The
 * split it came with is the reason it is worth keeping and is preserved exactly:
 *
 *   THIS FILE owns the format, the defaults, the ranges, what a corrupt file
 *   means, and what is written and what is omitted. It does NO I/O and knows
 *   nothing about retro-go, which is what lets tests/host_settings.c compile it
 *   with the host cc and grade every one of those rules with no target at all
 *   (tests/test_settings.py). Losing that property is losing the reason the
 *   design was worth copying.
 *
 *   src/main.c owns the other half: work out the path, open, read, close, hand
 *   the bytes over; and take the bytes back and write them.
 *
 * FILE HANDLES -- THE TRAP THAT MADE THIS A SEPARATE FILE. MAX_OPEN_FILES is 8
 * across the WHOLE firmware. This core already holds handles for its entire run
 * (the .dsk, and the pagefile), and exhaustion is SILENT: every other fopen()
 * in the firmware starts returning NULL and the UI degrades with nothing
 * logged. The prior port records the whole launcher rendering as diamonds
 * because of it. Keeping all I/O out of here keeps "open, read, close" in one
 * small place that can be read in one sitting.
 *
 * Freestanding: string.h and the one printf on the diagnostic paths. No malloc,
 * no float, no stdio beyond that.
 *
 *
 * ---- THE THREE LAYERS ------------------------------------------------------
 *
 *   1. the COMPILED-IN default -- today's behaviour, by construction. A title
 *      with no files of any kind gets exactly this, which is what makes "this
 *      change moves nothing that was working" checkable rather than hoped for.
 *   2. the BASE, set from `<TITLE>.cfg` by x86_cfg.c. Human-authored text that
 *      ships beside the .dsk. It does not "set" a setting; it REPLACES THE
 *      DEFAULT for this title.
 *   3. the user's `<TITLE>.dosset`, parsed here. It carries only what differs
 *      from the default -- it is just that the default it differs from is now
 *      the .cfg's.
 *
 * Expressing the .cfg as a DEFAULT rather than as a VALUE is the whole design,
 * and every property below falls out of it rather than being coded for: the
 * menu row still wins because the user pressing right is layer 3; a .dosset
 * never records a value the .cfg already says, so editing the .cfg still moves
 * every title nobody overrode by hand; and a corrupt or absent .cfg costs
 * exactly the compiled-in defaults with no separate failure path to get wrong.
 *
 *
 * ---- WHY TWO FILES AND NOT ONE ---------------------------------------------
 *
 * `.cfg` is a HUMAN DECISION about a title: hand-authored text, shipped beside
 * the image, never written by the device. `.dosset` is THE USER'S OVERRIDES,
 * written by the device when somebody changes a row in the PAUSE/SET menu.
 * Different producer, different lifetime. Merging them would mean the device
 * rewriting a file a human authored -- comments, ordering and all -- which is
 * how a hand-written config silently loses its comments.
 *
 * `.dosmeta`, the third sidecar, is untouched by any of this and nothing here
 * reads it.
 */

#ifndef X86_SETTINGS_H
#define X86_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* "XSET", little-endian. */
#define X86_SETTINGS_MAGIC   0x54455358UL
#define X86_SETTINGS_VERSION 1UL

/*
 * ---- The settings themselves ----------------------------------------------
 *
 * THE ENUMERATOR VALUES ARE THE WIRE FORMAT. They are written into the file as
 * the id half of each pair, so they may be APPENDED to and never reordered,
 * renumbered or reused. A removed setting's number is retired, not recycled: an
 * old file still containing it is skipped as unknown, and reusing the number
 * would silently re-interpret somebody's stale value as a different setting.
 *
 * UNKNOWN IDS ARE SKIPPED, NOT REJECTED, and are PRESERVED on the next save.
 * That is what makes downgrade safe: firmware that predates a setting reads a
 * file written by firmware that has it, ignores the pair it does not recognise,
 * and does not lose the user's choices when they upgrade again.
 */
typedef enum {
    /*
     * Index into src/main.c's cpu_profile_kips[] / cpu_profile_name[]. The one
     * genuinely settable Options row this core has today, and the reason this
     * whole feature exists: before this it persisted through
     * odroid_settings_app_int32_set("Palette"), whose key is scoped to the APP
     * and not the ROM -- i.e. ONE speed shared by every DOS title on the card.
     *
     * STORED AS AN INDEX, and the upper bound is deliberately looser than
     * CPU_PROFILE_COUNT-1: this module must not carry a copy of another file's
     * table length. The range here is a sanity bound; the CONSUMER still
     * validates the index against its own table before indexing with it.
     * Default 0 = "Max", which is what cpu_profile_init() defaulted to.
     */
    X86_SET_CPU_PROFILE = 0,

    /*
     * Pass the command tail AUTOEXEC.BAT named, or launch the program bare.
     * Mirrors the existing `guest_use_cmdtail` knob, which until now was
     * reachable only over the debug backend -- so a title needing (or needing
     * to avoid) its arguments could not be set up from the card at all.
     * Default 0, which is exactly what guest_use_cmdtail is initialised to.
     */
    X86_SET_USE_CMDTAIL = 1,

    /*
     * KB of extended memory to advertise, and KB of expanded memory.
     *
     * "UNSPECIFIED" IS CARRIED BY base_is_set(), NOT BY THE VALUE 0. That is
     * what base_is_set() is for, and spelling it the other way round would make
     * `xms = false` -- which a DOSBox-shaped .cfg must be able to say --
     * indistinguishable from saying nothing. So:
     *
     *   no .cfg key          -> base_is_set() 0, consumer leaves today's
     *                           X86_XMS_DEFAULT_KB / X86_EMS_PAGES alone
     *   `xms = false`        -> base_is_set() 1, value 0, advertise NONE
     *   `xms_kb = 2048`      -> base_is_set() 1, value 2048
     *
     * An EMS logical page is 16 KB and a page frame is all-or-nothing, so the
     * granularity rule is enforced by the .cfg value parser rather than by this
     * range -- 0 has to stay legal while 8 and 24 must not.
     */
    X86_SET_XMS_KB = 2,
    X86_SET_EMS_KB = 3,

    /*
     * DOSBox's `[cpu] cycles`, in THOUSANDS OF GUEST INSTRUCTIONS PER SECOND.
     *
     * The units line up by luck and it is worth stating why, because it is what
     * makes copying a number out of a dosbox.conf a sane thing to do: DOSBox
     * counts `cycles` as guest instructions per MILLISECOND, and this core
     * counts `guest_kips` as thousands of guest instructions per SECOND. Those
     * are the same number.
     *
     * SEPARATE FROM X86_SET_CPU_PROFILE ON PURPOSE, and this is the one place
     * the design departs from the prior port's. Its header flagged the question
     * and left it open: an INDEX into somebody else's table turns into a
     * different answer the day that table gains or reorders a row, which is why
     * its screen-rate setting persists the RATE and not the index. A rate is
     * self-describing; an index is a reference into a table this module is
     * forbidden to carry a copy of.
     *
     * So both exist and they do different jobs. The PROFILE index is what the
     * PAUSE/SET row moves and what the .dosset records, because that is
     * literally what the user changed. CYCLES is what a hand-written .cfg says,
     * and the CONSUMER -- src/main.c, which owns the table -- resolves it to
     * the nearest rung. The consumer prefers an explicit profile index when a
     * .cfg gives both.
     *
     * 0 = unspecified, again via base_is_set(). `cycles = max` does not land
     * here at all: it sets the PROFILE index to 0, which is what "Max" is.
     */
    X86_SET_CPU_CYCLES = 4,

    X86_SET_COUNT
} x86_setting_id_t;

/*
 * ---- Strings: `.cfg` ONLY, and deliberately not in the binary file ---------
 *
 * The launch parameters -- which program to run, and with what arguments -- are
 * text, and text is not something anybody sets by holding right on a d-pad. So
 * they live in the human-authored `.cfg` and have no representation in the
 * `.dosset` at all. That is not a gap: the .dosset exists to record what the
 * OPTIONS MENU changed, there is no menu row that could change a string, and
 * inventing a variable-length record for a value no writer can produce would be
 * format surface with no producer behind it.
 *
 * They are still parsed, range-checked and reported by this module, so the
 * whole of the format stays gradeable at tier 1 with no target.
 */
typedef enum {
    /* Program to run, replacing what the guest's own AUTOEXEC.BAT names. A path
       relative to the working directory below, or rooted with a leading / or \.
       Written either as `entry = FOO.EXE` or, DOSBox-style, as the command line
       in an `[autoexec]` section. */
    X86_STR_ENTRY = 0,
    /* The command tail handed to it. */
    X86_STR_ARGS  = 1,
    /* The working directory to run it from -- `[autoexec]`'s `cd` line, or
       `cwd = ` written out. Empty means the image root, which is what
       find_startup_program() starts from today. */
    X86_STR_CWD   = 2,
    X86_STR_COUNT
} x86_setting_str_id_t;

/* Bounded because nothing here allocates and the consumer copies into a fixed
   buffer anyway (src/main.c's `startup` is 64 and `cmdtail` is 64). A longer
   value is REFUSED and named, not truncated: half a command line is a
   different command line, and running one nobody wrote is worse than running
   the one AUTOEXEC.BAT named. */
#define X86_SETTINGS_STR_MAX 64

/*
 * The largest number of id/value pairs a file may contain. Bounded so a reader
 * can stack-allocate the whole thing; a file claiming more is refused rather
 * than trusted. 16 is X86_SET_COUNT with room to grow plus room to carry pairs
 * from a newer firmware across a downgrade.
 */
#define X86_SETTINGS_MAX_PAIRS 16

/* Header: magic, version, count, reserved. Then `count` 8-byte pairs. Then a
   CRC32 over everything before it. Byte-wise little-endian, NOT a struct
   memcpy: this is a wire format and one day something other than this compiler
   will write it. */
#define X86_SETTINGS_HDR_BYTES  16
#define X86_SETTINGS_PAIR_BYTES 8
#define X86_SETTINGS_MAX_BYTES  (X86_SETTINGS_HDR_BYTES \
                                 + X86_SETTINGS_MAX_PAIRS * X86_SETTINGS_PAIR_BYTES \
                                 + 4)

typedef enum {
    X86_SETTINGS_OK = 0,
    X86_SETTINGS_EABSENT,    /* zero bytes -- no file, which is not an error   */
    X86_SETTINGS_ENOTSET,    /* wrong magic, or too short to be a header       */
    X86_SETTINGS_EVERSION,   /* magic ok, version not understood               */
    X86_SETTINGS_ETOOMANY,   /* count exceeds X86_SETTINGS_MAX_PAIRS           */
    X86_SETTINGS_ESHORT,     /* count says more bytes than were handed over    */
    X86_SETTINGS_ECRC        /* it does not check                              */
} x86_settings_status_t;

const char *x86_settings_status_name(x86_settings_status_t s);

/* The short, stable name of a setting, for diagnostics. Never NULL. */
const char *x86_settings_name(x86_setting_id_t id);

/* ---- The base layer (the .cfg) ------------------------------------------- */

/* Drop every base, i.e. go back to the compiled-in defaults, and clear every
   string. MUST be called before parsing a title's .cfg, or the previous title's
   .cfg is still the default -- the same latch discipline reset() has, one layer
   down. x86_cfg_parse() calls it itself, on every path including the error
   paths, so there is no way to forget. */
void x86_settings_base_clear(void);

/* Make `value` the default for `id`. Returns 1 if it took, 0 if the id or the
   value is out of range (in which case the base is unchanged and the caller
   reports it -- this function prints nothing, because x86_cfg.c reports per
   FILE and not per key; the device log ring is small and wraps).

   DOES NOT TOUCH THE LIVE TABLE. Call x86_settings_reset() -- or load(), which
   resets first -- afterwards. That ordering is what lets a caller set every
   base and then apply them in one step. */
int x86_settings_base_set(x86_setting_id_t id, int value);

/* Is `id`'s default coming from a .cfg rather than from the firmware? A
   consumer asks this to distinguish "unspecified" from "specified as the same
   value the firmware would have picked". */
int x86_settings_base_is_set(x86_setting_id_t id);

/* The default in force for this title: the base if one was set, otherwise the
   compiled-in default. Also what get() answers for anything this module could
   not make sense of. With no base set -- every title with no .cfg -- this
   answers exactly what the firmware always did. */
int x86_settings_default(x86_setting_id_t id);

/* The default this FIRMWARE was compiled with, ignoring any .cfg. Only the
   diagnostic path and the tests want this. */
int x86_settings_builtin(x86_setting_id_t id);

/* The accepted range, for a parser that wants to reject a value before offering
   it. Returns 0 and leaves the two outputs alone for a bad id. */
int x86_settings_range(x86_setting_id_t id, int *lo, int *hi);

/* ---- Strings ------------------------------------------------------------- */

/* The string in force for `id`, or "" when none was given. NEVER NULL, so a
   consumer can hand it straight to strlen/strcmp without a guard -- a NULL here
   would be a fault on a cold path that only fires for titles that HAVE a .cfg,
   i.e. exactly the ones nobody tests. */
const char *x86_settings_str(x86_setting_str_id_t id);

/* Was one given at all? "" and "given as empty" are different: `args =` is a
   deliberate "run it with no arguments", which is not the same as saying
   nothing and letting AUTOEXEC.BAT decide. */
int x86_settings_str_is_set(x86_setting_str_id_t id);

/* Store `len` bytes at `s` as `id`. Returns 1 if it took, 0 if the id is
   unknown or the value does not fit in X86_SETTINGS_STR_MAX-1 bytes. REFUSED,
   NOT TRUNCATED -- see the constant. */
int x86_settings_str_set(x86_setting_str_id_t id, const char *s, unsigned long len);

/* The short, stable name of a string setting, for diagnostics. Never NULL. */
const char *x86_settings_str_name(x86_setting_str_id_t id);

/* ---- The live table ------------------------------------------------------ */

/* Drop everything back to defaults. Called by load() before it parses, so a
   second load cannot leave the previous title's answers behind. Does NOT clear
   the base: the base is set before the .dosset is read and has to survive the
   reset that reading it performs. */
void x86_settings_reset(void);

/*
 * Parse `len` bytes at `buf` into the live table.
 *
 * ON EVERY RETURN, INCLUDING EVERY ERROR, THE TABLE IS USABLE. An error means
 * "you got defaults", never "do not start" -- there is no return value a caller
 * is allowed to treat as fatal. Each distinct failure prints one line naming
 * itself, because a settings layer that quietly answers defaults is
 * indistinguishable from one that is working.
 *
 * len == 0 (no file on the card) is X86_SETTINGS_EABSENT and prints NOTHING: a
 * title that has never had its settings changed is the common case, not an
 * event.
 */
x86_settings_status_t x86_settings_load(const unsigned char *buf, unsigned long len);

/*
 * Serialise the live table into `buf` (room for X86_SETTINGS_MAX_BYTES) and
 * return the bytes written.
 *
 * ONLY NON-DEFAULT SETTINGS ARE WRITTEN, plus any unknown pairs carried over
 * from load(). Two consequences, both wanted: a title whose settings are all
 * default serialises to a header, a zero count and a CRC, so the file never
 * grows to describe things nobody chose; and CHANGING A DEFAULT IN FIRMWARE
 * MOVES EVERY TITLE THAT NEVER OVERRODE IT, which is the behaviour you want
 * from a default and would not get if save() wrote the whole table out.
 */
unsigned long x86_settings_save(unsigned char *buf, unsigned long cap);

/* Has anything been set() to a value different from what load() produced? The
   caller uses this to decide whether to write the file at all. THE GUARD IS NOT
   AN OPTIMISATION: without it every title anyone ever launched grows a .dosset
   recording nothing, and -- worse -- is pinned to today's defaults for ever,
   since a written default is indistinguishable from a chosen one. */
int x86_settings_dirty(void);

/* Read/write the live table. get() on an out-of-range id returns 0; set() on
   one is ignored. Neither can fault. */
int  x86_settings_get(x86_setting_id_t id);
void x86_settings_set(x86_setting_id_t id, int value);

/* CRC32, the ordinary reflected 0xEDB88320 one. Spelled out here rather than
   shared so this module links against NOTHING -- which is what lets
   tests/host_settings.c build it alone with the host cc. Twelve table-free
   lines are a cheaper dependency than a header between two freestanding
   modules. */
unsigned long x86_settings_crc32(const void *buf, unsigned long len);

#ifdef __cplusplus
}
#endif

#endif
