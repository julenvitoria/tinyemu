/*
 * DOS layer: memory arena, PSP, MZ loader and INT 21h.
 *
 * There is no DOS kernel in the guest. These services ARE DOS, so the things a
 * program can inspect have to be real rather than implied: the memory control
 * block chain is a genuine chain in guest memory, because programs walk it,
 * and the PSP is a real PSP because programs read their command tail and their
 * environment segment straight out of it.
 *
 * Anything not implemented records its function number and fails. A DOS call
 * that quietly returns success is indistinguishable from one that works until
 * the program acts on the result, and then the failure surfaces somewhere
 * unrelated -- the most expensive shape of bug this project has hit, three
 * times now.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_pager.h"       /* X86_PG_SIZE -- the granule XMS moves split on */
#include "x86_ems.h"          /* x86_ems_present() -- gates the EMMXXXX0 open */
#include "lib/FatFs/ff.h"

/*
 * Perform INT 21h AH=4Bh, or refuse it as an unimplemented function.
 *
 * DEFAULT 1 here and 0 in Makefile.core: the mechanism is built and tested at
 * tier 1 on every run, and the SHIPPING core keeps the behaviour it had until
 * the memory cost of a mid-run program load is solved. dos_exec_child() below
 * carries the measurement that decided it.
 */
#ifndef X86_DOS_EXEC
#define X86_DOS_EXEC 1
#endif

#define SEG(x)          ((uint32_t)(x) << 4)
#define PARA(bytes)     (((bytes) + 15u) >> 4)

/* Guest low memory:
 *   0000  interrupt vector table
 *   0400  BIOS data area
 *   0500  DOS scratch
 *   0600  our 0F 0B stubs (x86_hle.c)
 *   0700  first memory control block -- the arena starts here
 *   9FFF  end of conventional memory
 */
#define ARENA_FIRST_MCB X86_DOS_ARENA_SEG
#define ARENA_END_SEG   0x9FFFu

#define MCB_MEMBER      0x4Du       /* 'M': another block follows */
#define MCB_LAST        0x5Au       /* 'Z': end of the chain      */

/*
 * owner == 0 is FREE, and that is not ambiguous -- it is provably unusable as
 * a real owner. An owner field holds a PSP SEGMENT, and a PSP is the first
 * paragraph of the DATA area of an allocated block, so it is always
 * `mcb_seg + 1` for some block in the chain. The chain starts at
 * ARENA_FIRST_MCB (0x0070) and every block sits at or above it, so the
 * smallest owner value any block can ever carry is 0x0071. Segment 0 is the
 * interrupt vector table; nothing is ever allocated there, and DOS itself uses
 * the same encoding for the same reason.
 *
 * What IS a real hazard is a CALLER passing 0 as the owner and meaning "I will
 * fill this in later" -- the program loader did exactly that, and the block
 * holding the running program stayed on the free list until the next AH=48h
 * handed it back out. mcb_alloc() now refuses to record 0: an allocation is by
 * definition not free, so owner 0 is normalised to MCB_OWNER_DOS at the single
 * point where it would otherwise enter the chain.
 */
#define MCB_FREE        0x0000u     /* owner value meaning "on the free list" */
#define MCB_OWNER_DOS   0x0008u     /* blocks DOS itself owns (env, loader)   */

uint32_t x86_dos_last_error;        /* host-visible: last failing function */

/* --------------------------------------------------------------------------
 * Memory control blocks.
 *
 * Each block is a 16-byte header in guest memory followed by the memory it
 * describes; the data starts one paragraph after the header. Walking is
 * therefore: next = this + 1 + size.
 */
typedef struct {
    uint8_t  type;
    uint16_t owner;
    uint16_t size;                  /* paragraphs, excluding the header */
} mcb_t;

/* Chain-walk cost, in header reads. Every one of these is an x86_guest_ptr()
   call, and this counter is the whole reason the arena's cost is a number
   rather than an opinion: the recon pass found the host PC sitting in
   mcb_read/x86_guest_ptr and the guest down to ~40 KIPS, and "it feels faster"
   is not a measurement. Host-visible; four bytes. */
uint32_t x86_mcb_reads;

static int mcb_read(uint16_t seg, mcb_t *m)
{
    x86_mcb_reads++;
    uint8_t *p = x86_guest_ptr(SEG(seg), 16);
    if (!p)
        return 0;
    m->type = p[0];
    m->owner = (uint16_t)(p[1] | (p[2] << 8));
    m->size = (uint16_t)(p[3] | (p[4] << 8));
    return m->type == MCB_MEMBER || m->type == MCB_LAST;
}

static void mcb_write(uint16_t seg, const mcb_t *m)
{
    uint8_t *p = x86_guest_ptr(SEG(seg), 16);
    if (!p)
        return;
    p[0] = m->type;
    p[1] = (uint8_t)m->owner;
    p[2] = (uint8_t)(m->owner >> 8);
    p[3] = (uint8_t)m->size;
    p[4] = (uint8_t)(m->size >> 8);
    memset(p + 5, 0, 11);
}

void x86_dos_arena_init(void)
{
    mcb_t m;
    m.type = MCB_LAST;
    m.owner = MCB_FREE;
    m.size = (uint16_t)(ARENA_END_SEG - ARENA_FIRST_MCB - 1u);
    mcb_write(ARENA_FIRST_MCB, &m);
}

/* Merge each run of adjacent free blocks into one. Done before every
   allocation so a program that frees in pieces can still allocate the whole
   again -- without it the arena fragments permanently. */
static void mcb_coalesce(void)
{
    uint16_t seg = ARENA_FIRST_MCB;
    mcb_t m, n;
    while (mcb_read(seg, &m)) {
        if (m.type == MCB_LAST)
            break;
        uint16_t next = (uint16_t)(seg + m.size + 1u);
        if (m.owner == MCB_FREE && mcb_read(next, &n) && n.owner == MCB_FREE) {
            m.size = (uint16_t)(m.size + n.size + 1u);
            m.type = n.type;
            mcb_write(seg, &m);
            continue;               /* try to absorb the one after that too */
        }
        seg = next;
    }
}

/* Split `m` (already read, sitting at `seg`) down to `paras` paragraphs,
   handing the remainder back as a free block. Writes `m`. The tail absorbs the
   block after it if that one is free too, so a shrink never leaves two
   adjacent free blocks behind -- which is what made a later grow fail for no
   reason. Local, so it costs two reads instead of a whole-chain coalesce. */
static void mcb_shrink_to(uint16_t seg, mcb_t *m, uint16_t paras)
{
    if (m->size <= paras + 1u)
        return;                     /* not worth a header; keep the slack */

    uint16_t tail = (uint16_t)(seg + paras + 1u);
    mcb_t t;
    t.type = m->type;
    t.owner = MCB_FREE;
    t.size = (uint16_t)(m->size - paras - 1u);

    if (t.type != MCB_LAST) {
        mcb_t n;
        uint16_t after = (uint16_t)(tail + t.size + 1u);
        if (mcb_read(after, &n) && n.owner == MCB_FREE) {
            t.size = (uint16_t)(t.size + n.size + 1u);
            t.type = n.type;
        }
    }
    mcb_write(tail, &t);

    m->type = MCB_MEMBER;
    m->size = paras;
    mcb_write(seg, m);
}

/* Allocate `paras` paragraphs. Returns the DATA segment, or 0 with *largest
   set to the biggest block available -- which is what INT 21h 48h must report
   so a program can retry with a size that fits.
 *
 * One pass. It used to be three -- mcb_coalesce(), then the fitting walk, then
 * mcb_largest_free() -- each an O(n) chain walk through x86_guest_ptr() per
 * node. With a fragmented arena that is where the emulator was spending
 * essentially all of its time (measured: host PC in mcb_alloc/mcb_read/
 * x86_guest_ptr, guest down to ~40 KIPS). Merging free neighbours as the walk
 * passes them, and tracking the largest while it goes, gets the same answers
 * for a third of the reads and no extra state.
 */
static uint16_t mcb_alloc(uint16_t paras, uint16_t owner, uint16_t *largest)
{
    if (owner == MCB_FREE)
        owner = MCB_OWNER_DOS;      /* an allocation is never free; see above */

    uint16_t seg = ARENA_FIRST_MCB, best = 0;
    mcb_t m, n;
    while (mcb_read(seg, &m)) {
        /* Absorb the whole run of free blocks that follows this free one. */
        while (m.owner == MCB_FREE && m.type != MCB_LAST &&
               mcb_read((uint16_t)(seg + m.size + 1u), &n) &&
               n.owner == MCB_FREE) {
            m.size = (uint16_t)(m.size + n.size + 1u);
            m.type = n.type;
            mcb_write(seg, &m);
        }
        if (m.owner == MCB_FREE) {
            if (m.size > best)
                best = m.size;
            if (m.size >= paras) {
                mcb_shrink_to(seg, &m, paras);
                m.owner = owner;
                mcb_write(seg, &m);
                return (uint16_t)(seg + 1u);
            }
        }
        if (m.type == MCB_LAST)
            break;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    if (largest)
        *largest = best;
    return 0;
}

static int mcb_free(uint16_t data_seg)
{
    uint16_t seg = (uint16_t)(data_seg - 1u);
    mcb_t m;
    if (!mcb_read(seg, &m))
        return 0;
    m.owner = MCB_FREE;
    mcb_write(seg, &m);
    mcb_coalesce();
    return 1;
}

/* Grow or shrink in place. Growing only succeeds if the next block is free and
   big enough -- DOS does not move memory, and a program that assumed it did
   would already be broken on real hardware. */
static int mcb_resize(uint16_t data_seg, uint16_t paras, uint16_t *largest)
{
    uint16_t seg = (uint16_t)(data_seg - 1u);
    mcb_t m;
    if (!mcb_read(seg, &m))
        return 0;

    if (paras <= m.size) {
        mcb_shrink_to(seg, &m, paras);
        return 1;
    }

    /*
     * Grow. PROBE FIRST, COMMIT ONLY ON SUCCESS.
     *
     * This used to merge the next free block in and then recurse, and if the
     * result was still too small it returned 0 with the merge already written.
     * A failed AH=4Ah therefore ATE the free block it had just been told was
     * not enough -- while reporting failure. DOS/4GW probes for the largest
     * available block with BX=0xFFFF, gets "no", and asks again; each "no"
     * consumed more of the arena, so the loop never converged (measured on
     * STAR_WARS_DARKFORCES: 34,234 resizes, zero frees, arena marched to
     * segment 0x896A and the chain walk swamped the emulator).
     *
     * The walk below only READS. It also spans the entire run of adjacent free
     * blocks rather than just the first one, so a grow no longer fails merely
     * because the free space either side of a boundary had not been coalesced.
     */
    uint32_t avail = m.size;
    uint16_t last_type = m.type, next = (uint16_t)(seg + m.size + 1u);
    mcb_t n;
    while (last_type != MCB_LAST && mcb_read(next, &n) && n.owner == MCB_FREE) {
        avail += 1u + n.size;
        last_type = n.type;
        next = (uint16_t)(next + n.size + 1u);
    }

    if (avail < paras) {
        /* DOS reports the largest size this block CAN grow to. Nothing has
           been written: the chain is byte-identical to what we found. */
        if (largest)
            *largest = (uint16_t)(avail > 0xFFFFu ? 0xFFFFu : avail);
        return 0;
    }

    m.size = (uint16_t)avail;
    m.type = last_type;
    mcb_shrink_to(seg, &m, paras);
    mcb_write(seg, &m);
    return 1;
}

/* Test shims. The arena functions are static on purpose -- nothing outside
   INT 21h should allocate -- but the tier-1 harness has to drive them. */
uint16_t dos_test_alloc(uint16_t paras, uint16_t *largest)
{ return mcb_alloc(paras, 0x1234, largest); }
/* Deliberately reachable with owner 0: the point is to prove that a caller who
   asks for a block "to fill the owner in later" cannot leave it on the free
   list. Nothing but the test uses this. */
uint16_t dos_test_alloc_owner(uint16_t paras, uint16_t owner, uint16_t *largest)
{ return mcb_alloc(paras, owner, largest); }
uint16_t dos_test_owner_of(uint16_t data_seg)
{
    mcb_t m;
    if (!mcb_read((uint16_t)(data_seg - 1u), &m))
        return 0xFFFFu;
    return m.owner;
}
uint16_t dos_test_free(uint16_t seg)   { return (uint16_t)mcb_free(seg); }
uint16_t dos_test_resize(uint16_t seg, uint16_t paras, uint16_t *largest)
{ return (uint16_t)mcb_resize(seg, paras, largest); }

/* Walk the whole chain and report how many blocks it has, or 0 if it is
   malformed -- a chain that does not terminate is the failure mode that
   silently eats memory. */
uint16_t dos_test_walk(void)
{
    uint16_t seg = ARENA_FIRST_MCB, n = 0;
    mcb_t m;
    while (mcb_read(seg, &m)) {
        if (++n > 64)
            return 0;
        if (m.type == MCB_LAST)
            return n;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Program Segment Prefix.
 */
static uint16_t psp_seg;
static uint32_t dta_lin;            /* disk transfer area, default PSP:0080 */

/*
 * Build a PSP.
 *
 * `parent` is the PSP segment of the program that started this one, and it is
 * not decoration: AH=4Ch finds the process to resume through PSP:0016, so a
 * child whose link is wrong resumes the wrong program and one with a zero link
 * resumes segment 0 -- the interrupt vector table. That is the exact shape of
 * the derail Dark Forces produced when its EXEC went unanswered. The root
 * program is its own parent, which is what real DOS records too.
 * (MS-DOS 2.11 PSP layout; references/dosbox-x src/dos/dos_execute.cpp,
 * DOS_Execute() -> DOS_PSP::SetParent, and src/dos/dos_programs.cpp.)
 *
 * `tail` is passed with an explicit LENGTH rather than as a C string: the tail
 * an EXEC caller supplies comes out of guest memory verbatim and DOS copies
 * the byte count it was given, not up to the first NUL.
 */
static void psp_build(uint16_t seg, uint16_t mem_top,
                      const char *tail, unsigned tail_len,
                      uint16_t parent, uint16_t env_seg,
                      const uint8_t *fcbs)
{
    /* Built in a local and copied granule-aware: a PSP is placed at an
       arbitrary PARAGRAPH, so 256 bytes from it straddles a 4 KB granule
       whenever the segment lands in the last 240 bytes of a page --
       x86_guest_ptr() refuses that span and the PSP was then never written at
       all, in silence. Same defect class as the DTA. */
    uint8_t p[256];
    memset(p, 0, 256);

    p[0] = 0xCD; p[1] = 0x20;                   /* INT 20h */
    p[2] = (uint8_t)mem_top;                    /* first segment past us */
    p[3] = (uint8_t)(mem_top >> 8);
    p[5] = 0x9A;                                /* far call to the dispatcher */
    p[0x0A] = 0x00; p[0x0B] = 0x00;             /* INT 22h terminate vector */
    p[0x16] = (uint8_t)parent;                  /* parent PSP -- see above */
    p[0x17] = (uint8_t)(parent >> 8);
    p[0x2C] = (uint8_t)env_seg;                 /* environment block */
    p[0x2D] = (uint8_t)(env_seg >> 8);
    p[0x50] = 0xCD; p[0x51] = 0x21; p[0x52] = 0xCB;  /* INT 21h / RETF */

    /* Job file table. Twenty entries, 0xFF = closed, with the five standard
       devices already open, plus the far pointer at 0x34 that a runtime reads
       to find it. Ours is not a real SFT index table -- handles are resolved by
       handle_get() -- but a program that inspects the table to count its free
       handles must not read 256 open files. */
    memset(p + 0x18, 0xFF, 20);
    p[0x18] = 0x01; p[0x19] = 0x01; p[0x1A] = 0x01;  /* stdin/out/err */
    p[0x1B] = 0x00; p[0x1C] = 0x00;                  /* AUX, PRN */
    p[0x32] = 20;   p[0x33] = 0;
    p[0x34] = 0x18; p[0x35] = 0x00;
    p[0x36] = (uint8_t)seg; p[0x37] = (uint8_t)(seg >> 8);

    /* The two parsed FCBs an EXEC caller hands us. Nothing here parses a
       command tail into an FCB, so with no caller-supplied pair they stay
       zeroed rather than being invented. */
    if (fcbs) {
        memcpy(p + 0x5C, fcbs, 16);
        memcpy(p + 0x6C, fcbs + 16, 16);
    }

    unsigned n = tail_len;
    if (n > 126) n = 126;
    p[0x80] = (uint8_t)n;
    if (n) memcpy(p + 0x81, tail, n);
    p[0x81 + n] = 0x0D;

    x86_guest_copy_in(SEG(seg), p, 256);

    psp_seg = seg;
    dta_lin = SEG(seg) + 0x80u;
}

uint16_t x86_dos_psp(void) { return psp_seg; }

/* --------------------------------------------------------------------------
 * File handles. DOS handles 0-4 are the standard devices; real files start at
 * 5 and index the table below.
 */
#define DOS_MAX_FILES 16
static FIL dos_files[DOS_MAX_FILES];
static uint8_t dos_file_used[DOS_MAX_FILES];

static int handle_alloc(void)
{
    for (int i = 0; i < DOS_MAX_FILES; i++)
        if (!dos_file_used[i]) { dos_file_used[i] = 1; return i; }
    return -1;
}

static FIL *handle_get(uint16_t h)
{
    if (h < 5 || h - 5 >= DOS_MAX_FILES)
        return NULL;
    return dos_file_used[h - 5] ? &dos_files[h - 5] : NULL;
}

/*
 * Character devices.
 *
 * DOS programs open devices by name -- a full-screen UI typically does
 * Rewrite() on 'CON' and then writes through AH=40h rather than AH=02h, and a
 * report writer opens 'PRN' or 'NUL' the same way. There is no directory entry
 * to find, so these can never come from FatFs; they get handles out of a
 * reserved range instead, which cannot collide with the FatFs slots at 5..20.
 *
 * The standard handles are the same devices under their inherited numbers:
 * 0/1/2 are CON, 3 (AUX) and 4 (PRN) are sinks. Reads and writes on handle 1
 * previously fell into handle_get(), which rejects anything below 5, so a
 * program printing through the handle API got an "invalid handle" error for
 * every line it wrote.
 */
enum { DOS_DEV_NONE = 0, DOS_DEV_CON, DOS_DEV_NUL, DOS_DEV_EMS };
#define DOS_DEV_HANDLE(d)  ((uint16_t)(0x00F0 + (d)))

/* The name of the last file the guest asked us to create or open, for
   diagnosis: an unhandled request is far easier to place with the path in
   hand than with the function number alone. */
char x86_dos_last_path[32];

static int dos_name_is_device(const char *p)
{
    /* Compare the stem only: "CON", "CON:", "A:\CON" are all the device. */
    const char *s = p;
    for (const char *q = p; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':')
            s = q + 1;

    char n[5];
    unsigned i = 0;
    while (i < 4 && s[i] && s[i] != ':' && s[i] != '.') {
        char c = s[i];
        n[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        i++;
    }
    n[i] = 0;

    /* EMMXXXX0 -- the expanded-memory manager, and the ONE device name here
       that is eight characters rather than four, so it is compared against the
       whole stem before the truncated form below.

       It is a device only while EMS actually has backing. Answering the open
       with no expanded memory behind it is the exact failure x86_ems.h refuses:
       a title that gets a handle stops looking for another manager and then
       dies at its first INT 67h. With no backing the name falls through to
       FatFs, which cannot find it, and the guest gets CF=1/AX=2 -- "no such
       file", which is what "no driver" has always looked like here. */
    {
        char full[9];
        unsigned k = 0;
        while (k < 8 && s[k] && s[k] != ':' && s[k] != '.') {
            char c = s[k];
            full[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            k++;
        }
        full[k] = 0;
        if (k == 8 && !strcmp(full, "EMMXXXX0"))
            return x86_ems_present() ? DOS_DEV_EMS : DOS_DEV_NONE;
    }

    if (!strcmp(n, "CON"))
        return DOS_DEV_CON;
    if (!strcmp(n, "NUL") || !strcmp(n, "PRN") || !strcmp(n, "AUX") ||
        (i == 4 && (!strncmp(n, "LPT", 3) || !strncmp(n, "COM", 3))))
        return DOS_DEV_NUL;
    return DOS_DEV_NONE;
}

static int dos_handle_device(uint16_t h)
{
    if (h <= 2)
        return DOS_DEV_CON;                     /* stdin/stdout/stderr */
    if (h == 3 || h == 4)
        return DOS_DEV_NUL;                     /* AUX, PRN */
    if (h == DOS_DEV_HANDLE(DOS_DEV_CON))
        return DOS_DEV_CON;
    if (h == DOS_DEV_HANDLE(DOS_DEV_NUL))
        return DOS_DEV_NUL;
    if (h == DOS_DEV_HANDLE(DOS_DEV_EMS))
        return DOS_DEV_EMS;        /* opened only to be closed; see above */
    return DOS_DEV_NONE;
}

/* Pull a NUL-terminated guest string out, one page at a time. */
static void guest_str(uint32_t lin, char *out, unsigned max)
{
    unsigned i = 0;
    while (i < max - 1) {
        uint8_t *p = x86_guest_ptr(lin + i, 1);
        if (!p || *p == 0)
            break;
        out[i++] = (char)*p;
    }
    out[i] = 0;
}

/*
 * A DOS path is not a FatFs path.
 *
 * FatFs is built FF_STR_VOLUME_ID=0, so the only drive prefix it understands is
 * a DIGIT ("0:"). A guest naming its own file the way DOS programs habitually
 * do -- "A:DATABASE.INI", "C:\TB\DATABASE.INI" -- hands it a LETTER, which
 * f_open rejects outright with FR_INVALID_DRIVE. We have exactly one volume
 * (the mounted .dsk), so every drive letter means the same disk: strip it.
 *
 * FF_FS_RPATH is 0 too, so FatFs has no current directory of its own. That is
 * fine for a single-floppy title whose files all sit in the root, and WRONG for
 * a title AUTOEXEC.BAT reaches with `CD \DF`: every relative name it then opens
 * ("DARK.GOB") has to resolve against that directory, and with no CWD anywhere
 * in the stack it resolved against the root and missed. So the current
 * directory is kept HERE, as a plain prefix string, and applied to exactly the
 * paths DOS applies it to -- the relative ones. "\DF\TEXT.MSG" and "C:\FOO"
 * still mean the root, as they do under DOS.
 *
 * "C:FOO" is DRIVE-RELATIVE, not absolute: DOS resolves it against that
 * drive's current directory. We have one volume and one CWD, so it gets the
 * prefix too. (The reverted version of this change read "C:FOO" as rooted;
 * that is the wrong reading and it is the one a title in a subdirectory would
 * trip over.)
 */
static char dos_cwd[48];                        /* "" = root; no trailing '/' */

void x86_dos_set_cwd(const char *dir)
{
    unsigned i = 0;
    if (dir) {
        while (*dir == '\\' || *dir == '/')
            dir++;
        while (dir[i] && i < sizeof(dos_cwd) - 1) {
            dos_cwd[i] = (dir[i] == '\\') ? '/' : dir[i];
            i++;
        }
        while (i && dos_cwd[i - 1] == '/')
            i--;
    }
    dos_cwd[i] = 0;
}

const char *x86_dos_get_cwd(void) { return dos_cwd; }

static void dos_path_from_guest(uint32_t lin, char *out, unsigned max)
{
    char raw[64];
    guest_str(lin, raw, sizeof(raw));

    const char *s = raw;
    if (s[0] && s[1] == ':')                    /* drop any "X:" */
        s += 2;
    int absolute = (*s == '\\' || *s == '/');
    while (*s == '\\' || *s == '/')             /* and any leading separator */
        s++;

    unsigned i = 0;
    if (!absolute && dos_cwd[0]) {
        while (dos_cwd[i] && i < max - 2) {
            out[i] = dos_cwd[i];
            i++;
        }
        if (i < max - 1)
            out[i++] = '/';
    }
    unsigned j = 0;
    while (s[j] && i < max - 1) {
        out[i] = (s[j] == '\\') ? '/' : s[j];
        i++;
        j++;
    }
    out[i] = 0;
}

/* --------------------------------------------------------------------------
 * Directory search (AH=4Eh / 4Fh).
 *
 * FatFs is built FF_USE_FIND=0, so the pattern matching is ours. That is not a
 * loss: DOS wildcard rules are not FatFs's. A DOS name is a fixed 8+3 field,
 * '?' matches one padding-included position and '*' fills the REST OF THAT
 * FIELD -- so "*.INI" matches "DATABASE.INI" and "*" alone matches everything,
 * which is what a program enumerating a floppy actually sends.
 */
static char dos_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Expand "NAME.EXT" into the 11-character padded field DOS compares in. */
static void dos_83_field(const char *s, char out[11])
{
    unsigned i = 0;
    memset(out, ' ', 11);
    for (; *s && *s != '.' && i < 8; s++, i++)
        out[i] = dos_up(*s);
    while (*s && *s != '.')
        s++;
    if (*s == '.') {
        s++;
        for (i = 0; *s && i < 3; s++, i++)
            out[8 + i] = dos_up(*s);
    }
}

/* Same, but '*' fills the remainder of its own field with '?'. */
static void dos_83_pattern(const char *s, char out[11])
{
    unsigned i = 0;
    memset(out, ' ', 11);
    for (; *s && *s != '.' && i < 8; s++) {
        if (*s == '*') { while (i < 8) out[i++] = '?'; break; }
        out[i++] = dos_up(*s);
    }
    while (*s && *s != '.')
        s++;
    if (*s == '.') {
        s++;
        for (i = 0; *s && i < 3; s++) {
            if (*s == '*') { while (i < 3) out[8 + i++] = '?'; break; }
            out[8 + i++] = dos_up(*s);
        }
    }
}

static int dos_83_match(const char pat[11], const char name[11])
{
    for (unsigned i = 0; i < 11; i++)
        if (pat[i] != '?' && pat[i] != name[i])
            return 0;
    return 1;
}

/* One live search. DOS keeps its state in the first 21 bytes of the DTA; we
   keep ours here and stamp a serial into the DTA so a FindNext against a stale
   or foreign DTA fails cleanly instead of walking someone else's directory. */
static DIR dos_find_dir;
static char dos_find_pat[11];
static uint8_t dos_find_attr;
static uint8_t dos_find_open;
static uint8_t dos_find_serial;
#define DOS_FIND_MAGIC 0xF1

/* Fill the DTA's result half from a directory entry, and return 1. */
/*
 * The 8.3 name of a directory entry.
 *
 * With LFN enabled FatFs puts the short name in `altname` and leaves it EMPTY
 * when the long and short names are the same, so the fallback to `fname` is
 * required either way. With LFN disabled (which is how this FatFs is built --
 * it mounts FAT12 floppies, see lib/FatFs/ffconf.h) there is no `altname`
 * member at all and `fname` IS the 8.3 name. DOS only ever sees 8.3, so both
 * builds must land on the same string.
 */
#if FF_USE_LFN
#define DOS_SHORT_NAME(f)  ((f)->altname[0] ? (f)->altname : (f)->fname)
#else
#define DOS_SHORT_NAME(f)  ((f)->fname)
#endif

/*
 * The DTA is guest memory at an address the GUEST chose, so it can sit
 * anywhere -- including within 43 bytes of the end of a 4 KB granule.
 * `x86_guest_ptr()` refuses a span that crosses one and returns NULL, and
 * this function used to ask it for all 43 bytes at once and treat the refusal
 * as "no match". The caller cannot tell that from "the directory ended", so
 * the guest was told the disk was EMPTY. WOLF3D sets its DTA to DS:0xFFBA and
 * its findfirst("*.WL1") returned AX=0x12 with all eight data files present.
 *
 * `x86_guest_copy_in()` walks granules, so it is the only correct way to put a
 * multi-byte record into guest memory at an address we did not pick.
 *
 * The two writes are separate because bytes 2..0x14 are DOS's own search
 * state, which the caller may not clobber; only our magic/serial at 0..1 and
 * the result record at 0x15..0x2A belong to us.
 */
static int dos_find_report(const FILINFO *fno)
{
    uint8_t hdr[2] = { DOS_FIND_MAGIC, dos_find_serial };
    uint8_t rec[22];

    rec[0x00] = fno->fattrib;
    rec[0x01] = (uint8_t)fno->ftime;       rec[0x02] = (uint8_t)(fno->ftime >> 8);
    rec[0x03] = (uint8_t)fno->fdate;       rec[0x04] = (uint8_t)(fno->fdate >> 8);
    rec[0x05] = (uint8_t)fno->fsize;       rec[0x06] = (uint8_t)(fno->fsize >> 8);
    rec[0x07] = (uint8_t)(fno->fsize >> 16);
    rec[0x08] = (uint8_t)(fno->fsize >> 24);

    memset(rec + 0x09, 0, 13);
    const char *n = DOS_SHORT_NAME(fno);
    for (unsigned i = 0; n[i] && i < 12; i++)
        rec[0x09 + i] = (uint8_t)dos_up(n[i]);

    if (x86_guest_copy_in(dta_lin, hdr, sizeof(hdr)) != sizeof(hdr))
        return 0;
    if (x86_guest_copy_in(dta_lin + 0x15, rec, sizeof(rec)) != sizeof(rec))
        return 0;
    return 1;
}

/* Walk until an entry matches, or the directory ends. */
static int dos_find_step(void)
{
    FILINFO fno;
    for (;;) {
        if (f_readdir(&dos_find_dir, &fno) != FR_OK || fno.fname[0] == 0)
            return 0;
        /* A DOS search returns normal files always, and hidden/system/dir
           entries only if the caller asked for them in CX. */
        uint8_t extra = (uint8_t)(fno.fattrib & (AM_HID | AM_SYS | AM_DIR));
        if (extra & ~dos_find_attr)
            continue;
        char field[11];
        const char *n = DOS_SHORT_NAME(&fno);
        dos_83_field(n, field);
        if (dos_83_match(dos_find_pat, field))
            return dos_find_report(&fno);
    }
}

/* --------------------------------------------------------------------------
 * MZ loader.
 */
typedef struct {
    uint16_t sig, last_page, pages, relocs, hdr_paras, minalloc, maxalloc;
    uint16_t ss, sp, checksum, ip, cs, reloc_off, overlay;
} mz_header_t;

/* What a load produced: where the program went and how it must be entered.
   The loader does NOT touch the CPU -- the first program and an EXEC'd child
   need the same bytes in memory and completely different transfers into them,
   and a loader that also drove the CPU is what would have had to be written
   twice. */
typedef struct {
    uint16_t psp, load_seg, mem_top;
    uint16_t cs, ip, ss, sp;
} dos_prog_t;

/*
 * An MZ file opened and MEASURED, but not yet placed.
 *
 * THE SPLIT EXISTS BECAUSE THERE MUST BE ONLY ONE LOADER. AH=4Bh AL=00h and
 * AL=03h need the identical header parse and the identical image copy, and
 * differ entirely in the step BETWEEN them: AL=00h must allocate a block
 * before it can know where the image goes, while AL=03h (overlay) is TOLD
 * where by its caller and allocates nothing whatsoever. Cutting the loader at
 * exactly that seam lets both share the parts that are the same instead of
 * growing a second copy that drifts.
 */
typedef struct {
    FIL      f;
    mz_header_t h;
    uint32_t hdr_bytes;
    uint32_t image;             /* bytes of image AFTER the header */
} mz_open_t;

/*
 * Open an MZ .EXE and compute its header and image sizes. The file is left
 * OPEN on success -- mz_place() closes it, and every failure path here closes
 * it before returning.
 *
 * .COM images are NOT handled and are refused with error 0x0B, the same as any
 * other non-MZ file. Adding the .COM path is a separate, small change; saying
 * so is better than a loader that half-does it.
 */
static int mz_open(const char *path, mz_open_t *m)
{
    if (f_open(&m->f, path, FA_READ) != FR_OK) {
        x86_dos_last_error = 0x02;          /* file not found */
        return -1;
    }

    UINT got = 0;
    if (f_read(&m->f, &m->h, sizeof(m->h), &got) != FR_OK
        || got != sizeof(m->h)) {
        f_close(&m->f);
        x86_dos_last_error = 0x1E;
        return -1;
    }
    if (m->h.sig != 0x5A4D && m->h.sig != 0x4D5A) {
        f_close(&m->f);
        x86_dos_last_error = 0x0B;          /* invalid format */
        return -1;
    }

    m->hdr_bytes = (uint32_t)m->h.hdr_paras * 16u;
    m->image = (uint32_t)m->h.pages * 512u;
    if (m->h.last_page)
        m->image = m->image - 512u + m->h.last_page;
    if (m->image < m->hdr_bytes) {
        f_close(&m->f);
        x86_dos_last_error = 0x0B;
        return -1;
    }
    m->image -= m->hdr_bytes;
    return 0;
}

/*
 * Copy the image to load_seg:0000 and apply relocations, then close the file.
 *
 * THE TWO SEGMENTS ARE NOT THE SAME THING, and that is the whole content of
 * AL=03h. `load_seg` says where the bytes go. `reloc_factor` is the value
 * ADDED to every word the relocation table names. For a normal load they are
 * equal and the distinction is invisible; for an overlay the caller supplies
 * each independently, which is why the parameter block has two words.
 *
 * WHICH SEGMENT BIASES THE ADDRESS OF A RELOCATION ITEM -- the references
 * DISAGREE, and this follows real MS-DOS:
 *
 *   references/MS-DOS/v4.0/src/DOS/EXEC.ASM:515-527 loads SI = exec_rel_fac and
 *   uses it BOTH to reach the item (`MOV AX,DS / ADD AX,SI / MOV DS,AX`) and as
 *   the addend (`ADD [BX],SI`). v2.0/source/EXEC.ASM:632-641 is the same code.
 *   references/dosbox-x/src/dos/dos_execute.cpp:509-517 instead reaches the
 *   item through `RealSeg(relocpt)+loadseg` while adding `relocate`.
 *
 * Real MS-DOS is the authority on what a DOS program was linked against, so
 * the factor biases both. The two only differ when a caller passes a load
 * segment and a relocation factor that are not equal -- and a caller that does
 * that is asking for the image to be fixed up as though it lived at the
 * factor, which is precisely MS-DOS's reading.
 */
static int mz_place(mz_open_t *m, uint16_t load_seg, uint16_t reloc_factor)
{
    /* Copy the image in, a page at a time so it can cross granules.
     *
     * A SHORT COPY IS RECORDED, NOT SWALLOWED. x86_guest_copy_in() stops at
     * the first page the pager cannot resolve and returns how far it got, so
     * an image bigger than the memory available lands PARTIALLY and this loop
     * used to just stop -- leaving the tail of the program as whatever those
     * pages already held, with a success returned to the caller. The guest
     * then dies much later and somewhere else, executing 0xFF. That is the
     * single hardest failure in this file to diagnose from the outside, and
     * x86_dos_load_short is the number that names it. */
    f_lseek(&m->f, m->hdr_bytes);
    uint32_t done = 0;
    while (done < m->image) {
        uint8_t buf[512];
        UINT n = 0;
        UINT want = (m->image - done > sizeof(buf)) ? sizeof(buf)
                                                    : (UINT)(m->image - done);
        if (f_read(&m->f, buf, want, &n) != FR_OK || n == 0)
            break;
        if (x86_guest_copy_in(SEG(load_seg) + done, buf, n) != n)
            break;
        done += n;
    }
    /* Both describe the LAST load and are therefore assigned unconditionally:
       leaving _short at its previous value on a successful load would report a
       stale shortfall against an image that is completely resident, which is
       worse than not counting at all. _shorts is the cumulative one. */
    x86_dos_load_bytes = done;
    x86_dos_load_short = m->image - done;
    if (done < m->image)
        x86_dos_load_shorts++;

    /* Relocations: each entry names a word holding a segment, which has to be
       biased by the relocation factor. */
    if (m->h.relocs) {
        f_lseek(&m->f, m->h.reloc_off);
        for (unsigned i = 0; i < m->h.relocs; i++) {
            uint16_t ro[2];
            UINT n = 0;
            if (f_read(&m->f, ro, 4, &n) != FR_OK || n != 4)
                break;
            uint32_t lin = SEG((uint16_t)(reloc_factor + ro[1])) + ro[0];
            uint16_t *w = (uint16_t *)x86_guest_ptr(lin, 2);
            if (w)
                *w = (uint16_t)(*w + reloc_factor);
        }
    }
    f_close(&m->f);
    return 0;
}

/*
 * Load an MZ .EXE and build its PSP. Nothing else: no CPU state, no arena
 * bookkeeping beyond the block itself.
 */
static int dos_load_mz(const char *path, const char *tail, unsigned tail_len,
                       uint16_t parent, uint16_t env_seg, const uint8_t *fcbs,
                       dos_prog_t *out)
{
    mz_open_t m;
    if (mz_open(path, &m) != 0)
        return -1;
    mz_header_t h = m.h;
    uint32_t image = m.image;

    /*
     * One block for the PSP, the image and the memory the header asks for.
     *
     * BOTH bounds matter. minalloc is a minimum, not a suggestion: a program
     * that gets less overwrites whatever follows it. maxalloc is what the
     * program actually WANTS, and honouring only the minimum is not a
     * conservative choice -- it is a wrong one. A Borland runtime sizes its
     * heap from the block DOS hands it, and TOPBENCH ships maxalloc=0xFFFF,
     * i.e. "give me everything". Allocated at minalloc it started, found ~100 KB
     * where it expected ~600 KB, and died with `Runtime error 203` (heap
     * overflow) before printing a single result.
     *
     * Real DOS tries max first and falls back to the largest free block, which
     * is why 0xFFFF is the idiom for "all of it" rather than a request that
     * fails.
     */
    uint16_t need = (uint16_t)(PARA(256) + PARA(image) + h.minalloc);
    uint32_t want32 = (uint32_t)PARA(256) + PARA(image) + h.maxalloc;
    uint16_t want = (want32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)want32;
    if (want < need)
        want = need;

    /*
     * THE ACCEPTANCE TEST FOR THE GRANULE SEAM, and nothing else.
     *
     * A program's load address may only move by whole 4 KB granules while the
     * seam is broken: shift it by a granule and every structure keeps its
     * offset within its page, so the same accesses straddle and the same luck
     * holds; shift it by 256 bytes and a DIFFERENT set of accesses lands on a
     * boundary and the guest is silently corrupted. That is exactly how this
     * bug was found -- WOLF3D rendered at PSP 0x710 and at 0x1710, and broke at
     * 0x810 -- so a fix that cannot survive a non-granule-aligned PSP has not
     * been demonstrated.
     *
     * X86_DOS_PSP_SHIFT is paragraphs of dead block allocated ahead of the
     * program, plus the one paragraph of MCB header: 15 gives exactly 256
     * bytes. Default 0, so the shipping path is byte for byte what it was.
     * See tests/test_seam.py and the seam block in x86_seam.c.
     */
#if X86_DOS_PSP_SHIFT
    {
        uint16_t pad_largest = 0;
        (void)mcb_alloc(X86_DOS_PSP_SHIFT, MCB_OWNER_DOS, &pad_largest);
    }
#endif

    uint16_t largest = 0;
    uint16_t block = mcb_alloc(want, MCB_OWNER_DOS, &largest);
    uint16_t granted = want;
    if (!block) {
        uint16_t avail = largest;
        if (avail >= need) {
            block = mcb_alloc(avail, MCB_OWNER_DOS, &largest);
            granted = avail;
        }
    }
    if (!block) {
        f_close(&m.f);
        x86_dos_last_error = 0x08;          /* insufficient memory */
        return -1;
    }

    uint16_t psp = block;
    uint16_t load_seg = (uint16_t)(psp + PARA(256));

    /* The block belongs to the PROCESS, which is only known now -- mcb_alloc()
       above could not name a PSP that did not exist yet. This is what real DOS
       records, it is what AH=48h already records for a program's own
       allocations, and it is what lets a terminating child give its memory
       back by owner. Left as MCB_OWNER_DOS, an EXEC'd child would leak its
       entire block on exit and the second EXEC would find no memory. */
    {
        mcb_t hm;
        uint16_t hseg = (uint16_t)(block - 1u);
        if (mcb_read(hseg, &hm)) {
            hm.owner = psp;
            mcb_write(hseg, &hm);
        }
    }

    /* A normal load fixes the image up as though it lived where it was put --
       load segment and relocation factor are the same value. Only AL=03h ever
       separates them. */
    mz_place(&m, load_seg, load_seg);

    /* Top of memory as the program sees it: the end of what it was GRANTED,
       not of what it minimally needed. A Borland startup compares its own top
       against this field and refuses to run if it does not fit. */
    out->mem_top = (uint16_t)(psp + granted);
    psp_build(psp, out->mem_top, tail, tail_len, parent, env_seg, fcbs);

    /* Entry state, exactly as the header specifies. Segment arithmetic wraps
       at 16 bits; a base must be built from the wrapped value or it can land
       above 1 MB. */
    out->psp = psp;
    out->load_seg = load_seg;
    out->cs = (uint16_t)(load_seg + h.cs);
    out->ip = h.ip;
    out->ss = (uint16_t)(load_seg + h.ss);
    out->sp = h.sp;
    return 0;
}

/* The half of "enter a program" that is the same either way: its stack, and
   DS/ES on the PSP, which is where a program looks for its command tail. */
static void dos_prog_enter_data(const dos_prog_t *p)
{
    cpu_state_ptr->segs[2].sel  = p->ss;
    cpu_state_ptr->segs[2].base = SEG(p->ss);
    cpu_state_ptr->segs[2].limit = 0xFFFF;
    cpu_state_ptr->regs[4] = p->sp;

    for (int i = 0; i < 6; i += 3) {            /* ES (0) and DS (3) */
        cpu_state_ptr->segs[i].sel  = p->psp;
        cpu_state_ptr->segs[i].base = SEG(p->psp);
        cpu_state_ptr->segs[i].limit = 0xFFFF;
    }
    cpu_state_ptr->regs[0] = 0;
}

/*
 * The FIRST program is entered by setting CS:EIP outright -- there is no
 * interrupt in flight to return from, so nothing else could do it.
 *
 * An EXEC'd child must NOT be entered this way. It is reached from inside an
 * INT 21h, whose stub has not IRETed yet; overwriting CS there discards the
 * stub and the return path with it. dos_exec_child() leaves CS:EIP alone and
 * lets the IRET do the transfer. See the block comment above DOS_EXEC_DEPTH.
 */
static void dos_prog_enter(const dos_prog_t *p)
{
    dos_prog_enter_data(p);
    cpu_state_ptr->segs[1].sel  = p->cs;
    cpu_state_ptr->segs[1].base = SEG(p->cs);
    cpu_state_ptr->segs[1].limit = 0xFFFF;
    cpu_state_ptr->eip = p->ip;
}

static uint16_t dos_env_build(const char *path, uint16_t owner);

/* The root program's environment segment, 0 if none could be allocated. Read it
   to answer "does this guest have BLASTER" without guessing:
     python3 scripts/probe.py --watch x86_dos_env_seg */
volatile uint32_t x86_dos_env_seg = 0;

/* The FIRST program. Its parent is itself, as it is under real DOS -- there is
   no process above it to resume, and AH=4Ch at depth zero really does stop the
   machine. */
int x86_dos_exec(const char *path, const char *cmdline)
{
    dos_prog_t p;
    unsigned n = cmdline ? (unsigned)strlen(cmdline) : 0;
    uint16_t psp_guess = 0;

    /* The parent link has to name the block the loader is about to hand out,
       and only the loader knows which one that is. It fills PSP:0016 with the
       value passed here, so the root case is patched afterwards rather than
       predicted. */
    /*
     * THE ROOT PROGRAM HAD NO ENVIRONMENT AT ALL -- PSP:002C was zero and had
     * always been zero, because dos_env_build() was reached only from EXEC and
     * every title we ship is launched as the ROOT. So "BLASTER is not in the
     * environment" was true twice over, and writing the string without this
     * would have changed nothing observable for any title in the library.
     *
     * IT MUST BE ALLOCATED BEFORE THE PROGRAM, and that is not a preference.
     * dos_load_mz() hands an MZ with the usual 0xFFFF maxalloc EVERY FREE
     * PARAGRAPH there is, so an environment allocated afterwards finds nothing
     * and comes back 0 -- which is exactly what it did, measured at tier 2
     * (x86_dos_env_seg=0, WOLF3D still touching no 0x22x port at all). Real
     * DOS builds the environment first for the same reason.
     *
     * Owned by MCB_OWNER_DOS rather than by the PSP, because the PSP does not
     * exist yet. The root process never terminates into a parent that would
     * reclaim it, so nothing leaks that anything could notice; an EXEC'd child
     * still gets the PSP-owned block dos_exec_child() builds.
     *
     * A failure is not fatal: an environment is optional under DOS and a
     * program with none behaves exactly as it did before this.
     */
    x86_dos_env_seg = dos_env_build(path, MCB_OWNER_DOS);

    if (dos_load_mz(path, cmdline, n, psp_guess, (uint16_t)x86_dos_env_seg,
                    NULL, &p) != 0)
        return -1;
    {
        uint8_t *q = x86_guest_ptr(SEG(p.psp) + 0x16u, 2);
        if (q) { q[0] = (uint8_t)p.psp; q[1] = (uint8_t)(p.psp >> 8); }
    }
    dos_prog_enter(&p);
    return 0;
}

/* ==========================================================================
 * EXEC -- INT 21h AH=4Bh, and the AH=4Ch that comes back out of it.
 *
 * The load is dos_load_mz() above, unchanged and shared with the first
 * program. What is new here is only the RE-ENTRANCY: a second program running
 * inside the first one's machine, and a way back.
 *
 * THE WAY BACK IS THE PARENT'S OWN STACK. The INT 21h that reached us was
 * taken through the stub at 0060:xxxx (x86_hle.c), so the parent's SS:SP
 * already holds IP, CS, FLAGS and the stub ends in an IRET. Save SS:SP with
 * the rest of the parent's registers, hand the CPU to the child, and when the
 * child's AH=4Ch arrives put SS:SP back: the very same IRET then pops the
 * PARENT's frame and the parent resumes after its own INT 21h. Nothing has to
 * synthesise a return address, and no second control-transfer mechanism
 * exists to drift out of step with the first.
 *
 * The child is entered the same way, through that IRET, by building a frame on
 * ITS stack -- six bytes below the SS:SP its header asked for, which is its
 * own stack space and nobody else's.
 *
 * References: MS-DOS 2.11 (EXEC in MSDOS.ASM), and references/dosbox-x
 * src/dos/dos_execute.cpp DOS_Execute() / DOS_Terminate().
 */
#define DOS_EXEC_DEPTH 4

static void fail(uint16_t code);
static void ok_clear_cf(void);
static void dos_log_path(const char *p);

typedef struct {
    uint32_t regs[8];
    uint16_t sel[6];
    uint16_t psp;               /* the parent's PSP, and our psp_seg  */
    uint32_t dta;               /* the parent's DTA                   */
    uint16_t child_psp;
    uint16_t child_env;         /* the env block WE allocated, or 0   */
    uint16_t files;             /* handle slots the parent held       */
} dos_exec_frame_t;

static dos_exec_frame_t dos_exec_stack[DOS_EXEC_DEPTH];
static unsigned dos_exec_depth;
static uint16_t dos_ret_code;   /* what AH=4Dh reports, once          */

/* Host-visible, because "did the EXEC happen" is the first question asked of
   any run that reaches this code. */
volatile uint32_t x86_dos_execs;
volatile uint32_t x86_dos_exec_fails;
volatile uint32_t x86_dos_exec_depth_max;
volatile uint32_t x86_dos_exec_last_err;

/* AL=03h overlay loads. Separate from x86_dos_execs because an overlay is not
   a process: nothing was allocated, nothing will terminate, and "did the
   overlay land, and where" is the only question worth asking of it. */
/* The MZ loader's own honesty. x86_dos_load_bytes is how much of the last
   image actually reached guest memory; x86_dos_load_short is how much of it
   did NOT, and x86_dos_load_shorts counts the loads that came up short.
   Non-zero means a program is resident only in part -- which the guest
   discovers much later, in an unrelated place, usually by executing 0xFF. */
volatile uint32_t x86_dos_load_bytes;
volatile uint32_t x86_dos_load_short;
volatile uint32_t x86_dos_load_shorts;

volatile uint32_t x86_dos_overlays;
volatile uint32_t x86_dos_overlay_seg;
volatile uint32_t x86_dos_overlay_reloc;

/* Free every block the terminating process owns. DOS does this on exit and it
   is not optional here: the child's block is the largest thing in the arena
   and a parent that EXECs in a loop would otherwise run out on the second
   pass. */
static void mcb_free_owner(uint16_t owner)
{
    uint16_t seg = ARENA_FIRST_MCB;
    mcb_t m;
    while (mcb_read(seg, &m)) {
        if (m.owner == owner) {
            m.owner = MCB_FREE;
            mcb_write(seg, &m);
        }
        if (m.type == MCB_LAST)
            break;
        seg = (uint16_t)(seg + m.size + 1u);
    }
    mcb_coalesce();
}

#if X86_DOS_EXEC
static uint16_t dos_file_mask(void)
{
    uint16_t m = 0;
    for (int i = 0; i < DOS_MAX_FILES; i++)
        if (dos_file_used[i])
            m |= (uint16_t)(1u << i);
    return m;
}
#endif

int x86_dos_terminate_code(uint16_t code)
{
    if (dos_exec_depth) {
        /* A CHILD is exiting. Give the parent its machine back. */
        dos_exec_frame_t *fr = &dos_exec_stack[--dos_exec_depth];

        /* Close what the CHILD opened and nothing else. The parent's handles
           are still its own, and closing them here is the failure that would
           surface as the parent's next read returning "invalid handle" long
           after the EXEC. */
        for (int i = 0; i < DOS_MAX_FILES; i++)
            if (dos_file_used[i] && !(fr->files & (1u << i))) {
                f_close(&dos_files[i]);
                dos_file_used[i] = 0;
            }

        /* ONE call frees both: the child's program block and the environment
           block are both OWNED by the child's PSP, which is the only reason
           freeing by owner works at all. */
        mcb_free_owner(fr->child_psp);

        for (int i = 0; i < 8; i++)
            cpu_state_ptr->regs[i] = fr->regs[i];
        /* EVERY segment except CS. We are inside the child's INT 21h stub and
           its IRET has not run yet; that IRET is the transfer back, and it
           pops the parent's CS:IP off the stack we are restoring below. Write
           CS here and the next instruction is fetched from the parent's code
           segment at the STUB's offset -- which is how this first shipped, and
           it derails exactly the way an unanswered EXEC does. */
        for (int i = 0; i < 6; i++) {
            if (i == 1)
                continue;
            cpu_state_ptr->segs[i].sel  = fr->sel[i];
            cpu_state_ptr->segs[i].base = SEG(fr->sel[i]);
            cpu_state_ptr->segs[i].limit = 0xFFFF;
        }
        psp_seg = fr->psp;
        dta_lin = fr->dta;
        dos_ret_code = code;

        /* SS:SP is the parent's again, so THIS is now the parent's stacked
           FLAGS -- the frame its own INT 21h pushed. A successful EXEC returns
           with CF clear. */
        x86_set_ax(0);
        x86_hle_set_cf(0);
        return 1;                       /* keep running: as the parent */
    }

    /* Real DOS closes the process's handles on exit, and it matters here:
       FatFs is built FF_FS_TINY, so the last sector of a file the guest wrote
       is still sitting in the filesystem window until something flushes it. */
    for (int i = 0; i < DOS_MAX_FILES; i++)
        if (dos_file_used[i]) {
            f_close(&dos_files[i]);
            dos_file_used[i] = 0;
        }
    dos_ret_code = code;
    cpu_state_ptr->exit_reason = X86_EXIT_TERMINATE;
    return 1;
}

int x86_dos_terminate(void) { return x86_dos_terminate_code(0); }

/*
 * Build the child's environment block when the caller did not supply one.
 *
 * The layout is not ours to choose: strings, an extra NUL, a word of 1, then
 * the program's own full path. A protected-mode extender reads that path to
 * re-open the file it was loaded from, so an environment without it is worse
 * than none at all. Owned by the CHILD's PSP so it is freed with the child.
 *
 * BLASTER IS THE THIRD STRING AND IT IS NOT DECORATION. It is how a DOS title
 * finds the card: base, IRQ, 8-bit DMA channel, 16-bit DMA channel, type.
 * Wolfenstein 3D does not touch 0x22x at all without it (measured -- docs/03
 * 4a), so this variable is the switch that turns the whole Sound Blaster path
 * on for the library. It was deliberately withheld while a transfer could be
 * started and never finished; it is set now because it can finish.
 *
 * EVERY FIELD HERE IS A PROMISE x86_io.c HAS TO KEEP, and each one is checkable
 * against that file rather than copied off a forum post:
 *   A220  SB_BASE, the only base that answers
 *   I5    IRQ 5, the only line x86_sb_raise_irq() drives
 *   D1    8-bit DMA channel 1   -- sbp.dma8 default, and the mixer's 0x81 bit 1
 *   H5    16-bit DMA channel 5  -- sbp.dma16 default, and 0x81 bit 5
 *   T6    an SB16, which is what E1h answers with 4.05
 * Advertising anything we do not implement -- a T4 with no 16-bit channel, an
 * I7 nothing raises -- is worse than advertising nothing, because the driver
 * believes it and waits.
 */
#define DOS_ENV_BLASTER "BLASTER=A220 I5 D1 H5 T6"

static uint16_t dos_env_build(const char *path, uint16_t owner)
{
    uint8_t env[128];
    unsigned n = 0;
    static const char base[] =
        "PATH=\0COMSPEC=C:\\COMMAND.COM\0" DOS_ENV_BLASTER "\0";
    memcpy(env, base, sizeof(base) - 1);
    n = sizeof(base) - 1;
    env[n++] = 0;                       /* end of the strings */
    env[n++] = 1; env[n++] = 0;         /* one trailing item follows */
    env[n++] = 'C'; env[n++] = ':'; env[n++] = '\\';
    for (unsigned i = 0; path[i] && n < sizeof(env) - 1; i++)
        env[n++] = (uint8_t)dos_up(path[i]);
    env[n++] = 0;

    uint16_t largest = 0;
    uint16_t seg = mcb_alloc(PARA(n), owner, &largest);
    if (!seg)
        return 0;
    x86_guest_copy_in(SEG(seg), env, n);
    return seg;
}

/*
 * AH=4Bh. ES:BX is the parameter block:
 *
 *   +0  dw  environment segment, 0 = inherit the parent's
 *   +2  dd  far pointer to the command tail (a length byte, then the bytes)
 *   +6  dd  far pointer to the first  FCB
 *   +10 dd  far pointer to the second FCB
 *
 * ...but ONLY for AL=00h and AL=01h. AL=03h has a COMPLETELY DIFFERENT and
 * much shorter parameter block -- see dos_exec_overlay() below.
 *
 * AL=00 (load and execute) and AL=03 (load overlay) are implemented. AL=01
 * (load, do not execute) is NOT -- it is refused loudly through the same
 * unhandled-function path as any other missing service, because a program
 * whose overlay was silently not loaded fails somewhere unrelated.
 */

#if X86_DOS_EXEC
/*
 * AH=4Bh AL=03h -- LOAD OVERLAY.
 *
 * The easy sibling of AL=00h, and deliberately so: there is no child PSP, no
 * MCB allocation, no environment block, no parent link, no transfer of
 * control and no AH=4Ch return path. The caller has already decided where the
 * image goes and will CALL into it itself; all DOS does is put the bytes there
 * and fix them up.
 *
 * ES:BX is a TWO-WORD parameter block, not the fourteen-byte one AL=00h uses:
 *
 *   +0  dw  segment address where the file will be loaded
 *   +2  dw  relocation factor to be applied to the image
 *
 * references/MS-DOS/v2.0/source/EXEC.ASM:56-63 is the comment block that
 * documents exactly those two words; v4.0/src/DOS/EXEC.ASM:326-333 is the code
 * that reads them (`exec3_load_addr` -> exec_dma, `exec3_reloc_fac` ->
 * exec_rel_fac). references/dosbox-x/include/dos_inc.h:570-573 carries the
 * same pair as `struct sOverlay { uint16_t loadseg; uint16_t relocation; }`.
 *
 * NO MEMORY IS ALLOCATED AND NO BOUNDS ARE CHECKED. That is not an omission:
 * references/dosbox-x/src/dos/dos_execute.cpp:313-319 records that MS-DOS 4.0
 * "will blindly load the EXE image to whatever segment is given even if it
 * overruns one MCB into another", which is why titles rely on overlay loads
 * landing in a buffer they allocated themselves. We do the same and let the
 * caller own the memory.
 *
 * On success MS-DOS returns CF=0 with AX and DX BOTH ZERO
 * (dos_execute.cpp:823-826, confirmed there against MS-DOS 6.22), and
 * v4.0/src/DOS/EXEC.ASM:588-590 returns SYS_RET_OK the moment the overlay is
 * placed -- no PSP is built and no entry state is computed.
 */
static int dos_exec_overlay(const char *path)
{
    uint32_t *r = cpu_state_ptr->regs;

    /* Read granule-aware: ES:BX is an address the GUEST chose and even four
       bytes from it can straddle a 4 KB boundary, which x86_guest_ptr()
       refuses outright. */
    uint8_t pb[4];
    uint32_t pb_lin = cpu_state_ptr->segs[0].base + (uint16_t)r[3];
    if (x86_guest_copy_out(pb, pb_lin, sizeof(pb)) != sizeof(pb)) {
        x86_dos_exec_fails++;
        x86_dos_exec_last_err = 0x0B;
        fail(0x0B);
        return 1;
    }
    uint16_t load_seg     = (uint16_t)(pb[0] | (pb[1] << 8));
    uint16_t reloc_factor = (uint16_t)(pb[2] | (pb[3] << 8));

    mz_open_t m;
    if (mz_open(path, &m) != 0) {
        x86_dos_exec_fails++;
        x86_dos_exec_last_err = x86_dos_last_error;
        fail((uint16_t)x86_dos_last_error);
        return 1;
    }
    mz_place(&m, load_seg, reloc_factor);

    x86_dos_overlays++;
    x86_dos_overlay_seg = load_seg;
    x86_dos_overlay_reloc = reloc_factor;

    /* AX and DX are both zero on a successful overlay load, and CF is clear. */
    x86_set_ax(0);
    x86_set_dx(0);
    ok_clear_cf();
    return 1;
}
#endif /* X86_DOS_EXEC */

static int dos_exec_child(void)
{
    uint32_t *r = cpu_state_ptr->regs;
    uint8_t al = (uint8_t)r[0];

#if !X86_DOS_EXEC
    /*
     * THE REFUSAL ARM. X86_DOS_EXEC now defaults to 1 (Makefile.core), so this
     * is reached only by a build that deliberately compiles EXEC out -- which
     * is what tests/test_dos.py's negative controls do.
     *
     * IT WAS THE DEFAULT for several passes, on a measurement that DID NOT
     * SURVIVE RE-TESTING. The record said WOLFENSTEIN 3-D calls AH=4Bh AL=00h
     * with a loadable image and that honouring it killed the title
     * (guest_insns_total 0, guest_exit_reason 5 MEM_FAULT, no picture), the
     * mechanism being a child image copied into ~30 untouched guest pages with
     * the pager's pool already full. Re-measured at tier 2 on this tree:
     *
     *   WOLF3D DOES NOT CALL AH=4Bh AT ALL in the graded window.
     *   x86_dos_execs=0 and x86_dos_overlays=0 over 340 M instructions;
     *   x86_dos_load_bytes=109,927 is WOLF3D.EXE itself, loaded by the LAUNCH
     *   path (dos_load_mz() at the x86_dos_start() call site), which is not
     *   this switch. x86_pg_exhausted=0, evictions=0, load_shorts=0,
     *   x86_pg_frames_total=159 -- the same 159 the flag-off build gets, so
     *   the flag costs no frames either.
     *
     * The reported signature -- zero instructions with every other counter
     * healthy -- is the RAM_EMU starvation trap documented in STATUS.md's
     * Budgets row, which took out three instrumented builds in one session.
     * Attributing it to the loader cost this flag several passes; the number
     * that would have caught it is x86_dos_execs, which nobody read.
     *
     * Everything above this point is real and tested (tests/test_dos.py, its
     * negative controls); only the SWITCH is gated, and gated off it returns
     * CF=1, AX=0001 exactly as the default arm did before any of this work.
     */
    x86_hle_unhandled_vector = 0x21;
    x86_hle_unhandled_func = 0x4B;
    x86_dos_exec_last_err = 0x200u | al;
    x86_dos_exec_fails++;
    fail(0x01);
    return 0;
#else
    if (al != 0x00 && al != 0x03) {
        x86_hle_unhandled_vector = 0x21;
        x86_hle_unhandled_func = 0x4B;
        x86_dos_exec_last_err = 0x100u | al;    /* the SUBfunction, not 4B */
        fail(0x01);
        return 0;
    }

    /* DS:DX names the file for every subfunction, so it is parsed before the
       split. Everything AFTER the split is process machinery an overlay has
       no use for. */
    char path[64];
    dos_path_from_guest(cpu_state_ptr->segs[3].base + (uint16_t)r[2],
                        path, sizeof(path));
    strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
    dos_log_path(path);

    /* AL=03h leaves here. No depth check (an overlay does not nest -- it never
       takes the CPU), no parameter block beyond two words, no PSP, no frame. */
    if (al == 0x03)
        return dos_exec_overlay(path);

    if (dos_exec_depth >= DOS_EXEC_DEPTH) {
        x86_dos_exec_fails++;
        x86_dos_exec_last_err = 0x08;
        fail(0x08);
        return 1;
    }

    /* The parameter block. Read granule-aware: ES:BX is an address the GUEST
       chose and 14 bytes from it can straddle a 4 KB boundary, which
       x86_guest_ptr() refuses outright. */
    uint8_t pb[14];
    uint32_t pb_lin = cpu_state_ptr->segs[0].base + (uint16_t)r[3];
    if (x86_guest_copy_out(pb, pb_lin, sizeof(pb)) != sizeof(pb)) {
        x86_dos_exec_fails++;
        x86_dos_exec_last_err = 0x0B;
        fail(0x0B);
        return 1;
    }
    uint16_t env_seg = (uint16_t)(pb[0] | (pb[1] << 8));
    uint32_t tail_lin = ((uint32_t)(pb[4] | (pb[5] << 8)) << 4)
                      + (uint16_t)(pb[2] | (pb[3] << 8));
    uint32_t fcb1_lin = ((uint32_t)(pb[8] | (pb[9] << 8)) << 4)
                      + (uint16_t)(pb[6] | (pb[7] << 8));
    uint32_t fcb2_lin = ((uint32_t)(pb[12] | (pb[13] << 8)) << 4)
                      + (uint16_t)(pb[10] | (pb[11] << 8));

    uint8_t tail[128];
    unsigned tail_len = 0;
    if (x86_guest_copy_out(tail, tail_lin, 1) == 1) {
        tail_len = tail[0];
        if (tail_len > 126)
            tail_len = 126;
        if (tail_len &&
            x86_guest_copy_out(tail, tail_lin + 1, tail_len) != tail_len)
            tail_len = 0;
    }

    uint8_t fcbs[32];
    memset(fcbs, 0, sizeof(fcbs));
    (void)x86_guest_copy_out(fcbs, fcb1_lin, 16);
    (void)x86_guest_copy_out(fcbs + 16, fcb2_lin, 16);

    /* Inherit the parent's environment when the caller passed 0, which is what
       the field means. Only build one if there is nothing to inherit. */
    uint16_t built_env = 0;
    if (!env_seg) {
        uint8_t *pe = x86_guest_ptr(SEG(psp_seg) + 0x2Cu, 2);
        if (pe)
            env_seg = (uint16_t)(pe[0] | (pe[1] << 8));
    }

    /*
     * ORDER MATTERS: save the parent BEFORE the load. dos_load_mz() builds the
     * child's PSP, and psp_build() moves psp_seg and dta_lin to it -- so by the
     * time the load returns, the parent's own values are already gone.
     */
    dos_exec_frame_t *fr = &dos_exec_stack[dos_exec_depth];
    for (int i = 0; i < 8; i++)
        fr->regs[i] = cpu_state_ptr->regs[i];
    for (int i = 0; i < 6; i++)
        fr->sel[i] = cpu_state_ptr->segs[i].sel;
    fr->psp = psp_seg;
    fr->dta = dta_lin;
    fr->files = dos_file_mask();

    dos_prog_t p;
    if (dos_load_mz(path, (const char *)tail, tail_len, fr->psp, env_seg,
                    fcbs, &p) != 0) {
        /* Nothing was saved that needs undoing -- the frame is not committed
           until dos_exec_depth moves, and a failed load never reached
           psp_build(). x86_dos_last_error carries the DOS code the loader
           chose (2 = not found, 8 = no memory, 0x0B = bad format). */
        x86_dos_exec_fails++;
        x86_dos_exec_last_err = x86_dos_last_error;
        fail((uint16_t)x86_dos_last_error);
        return 1;
    }
    if (!env_seg) {
        built_env = dos_env_build(path, p.psp);
        uint8_t *q = x86_guest_ptr(SEG(p.psp) + 0x2Cu, 2);
        if (q) { q[0] = (uint8_t)built_env; q[1] = (uint8_t)(built_env >> 8); }
    }
    fr->child_psp = p.psp;
    fr->child_env = built_env;

    /*
     * The child's entry frame, six bytes below the stack its header asked for.
     * Granule-aware for the same reason as everything else that writes an
     * address the guest picked. FLAGS = 0x0200 (IF set, CF clear), which is
     * what a program entered from DOS finds.
     */
    uint16_t sp = (uint16_t)(p.sp - 6u);
    uint8_t frame[6] = {
        (uint8_t)p.ip, (uint8_t)(p.ip >> 8),
        (uint8_t)p.cs, (uint8_t)(p.cs >> 8),
        0x00, 0x02,
    };
    if (x86_guest_copy_in(SEG(p.ss) + sp, frame, 6) != 6) {
        /* The child's own stack is unreachable, so there is no way to enter
           it. Undo the allocation rather than leave a block owned by a process
           that will never run. */
        mcb_free_owner(p.psp);      /* the env block is owned by it too */
        psp_seg = fr->psp;
        dta_lin = fr->dta;
        x86_dos_exec_fails++;
        x86_dos_exec_last_err = 0x0B;
        fail(0x0B);
        return 1;
    }

    /* Stack, DS and ES only. CS:EIP still address the INT 21h stub, whose
       IRET is one instruction away and is what actually enters the child --
       off the frame written just above. */
    dos_prog_enter_data(&p);
    cpu_state_ptr->regs[4] = sp;        /* below the frame we just pushed */

    dos_exec_depth++;
    if (dos_exec_depth > x86_dos_exec_depth_max)
        x86_dos_exec_depth_max = dos_exec_depth;
    x86_dos_execs++;
    /* No CF to clear: the stacked FLAGS now belong to the CHILD's frame, and
       it was pushed with CF already clear. The parent's frame is untouched on
       its own stack, waiting for the AH=4Ch that will pop it. */
    return 1;
#endif /* X86_DOS_EXEC */
}

/* --------------------------------------------------------------------------
 * AH=52h -- the LIST OF LISTS (SysVars).
 *
 * WHY. A DOS extender does not ask what version we are and believe it; it asks
 * for the kernel's own variables and computes from them. DOS/4GW's stub issues
 * exactly one AH=52h before the far jump that took Dark Forces into unwritten
 * memory at linear 0x3E0C3 -- refused, CF set, and then an address derived from
 * whatever ES:BX happened to hold. A refusal here is not a no-op, it is a
 * pointer the guest uses anyway.
 *
 * LAYOUT. MS-DOS 4.0 is the source of record: SYSINITVAR is declared at
 * references/MS-DOS/v4.0/src/DOS/MSCONST.ASM:66-105 with the symbolic mirror at
 * references/MS-DOS/v4.0/src/INC/SYSVAR.INC:4-29, and function 52h itself is
 * references/MS-DOS/v4.0/src/DOS/MISC.ASM:145-155, which returns ES = the DOS
 * data segment and BX = OFFSET SYSINITVAR. `arena_head`, the WORD at -2, is
 * MSCONST.ASM:63. DOSBox-X agrees field for field from +0x00 to +0x45
 * (references/dosbox-x/include/dos_inc.h:616-668, filled in
 * src/dos/dos_classes.cpp:72-170, served at src/dos/dos.cpp:2433-2441), so the
 * two references do NOT disagree in the range anything here populates. Where
 * they do differ is +0x37/+0x3B -- MS-DOS 4.0 has the IFS chain, DOSBox-X the
 * DOS 5 SETVER pointer -- and we report DOS 5 (AH=30h above), so those follow
 * DOSBox-X and are zero either way. MS-DOS 2.0 (MSDATA.ASM:83-111) is a
 * genuinely different and incompatible layout and is NOT what we build.
 *
 * WHAT IS REAL AND WHAT IS REFUSED. The rule for this block is that a field is
 * either a value we can derive from something we actually have, or it is the
 * documented END-OF-CHAIN / absent marker. Nothing here is plausible filler:
 * the entire failure being fixed is a guest computing an address from a number
 * that looked reasonable and was not.
 *
 *   REAL   -2   first MCB     ARENA_FIRST_MCB -- the chain mcb_write() built
 *          0x10 max sector    512, the sector size x86_disk.c actually serves
 *          0x20 block devices 1  -- matches AH=0Eh/AH=19h, which know of one
 *          0x21 LASTDRIVE     1  -- likewise; A: is the only drive there is
 *          0x43 boot drive    1 = A:
 *          0x44 dword move    1  -- this is a 386 core
 *          0x45 extended KB   x86_xms_total_kb, the number XMS really reports
 *
 *   ABSENT 0x00 DPB chain     ) We model no drive parameter blocks, no system
 *          0x04 SFT chain     ) file tables, no disk buffers and no current
 *          0x08 CLOCK$ hdr    ) directory structures. Handles live in a host
 *          0x0C CON hdr       ) array and paths are resolved by FatFs, so there
 *          0x12 buffer head   ) is no guest-visible structure to point at. Each
 *          0x16 CDS array     ) is 0xFFFF:0xFFFF -- offset 0xFFFF is DOS's own
 *          0x1A FCB table     ) end-of-chain marker, so a guest that WALKS one
 *                               stops at once instead of dereferencing filler.
 *                               A guest that INDEXES one (a CDS lookup) gets an
 *                               unmapped segment and a clean fault, which is
 *                               the visible failure we want, not a silent one.
 *
 * The NUL device header at +0x22 is real as a header and honest as a chain: the
 * name, attribute word (DEVTYP OR ISNULL = 0x8004, MSCONST.ASM:81) and the
 * 0xFFFFFFFF next-link are what MS-DOS plants, and 0xFFFFFFFF means the chain
 * ends here -- which is true, because we have no loaded device drivers.
 * KNOWN GAP: the EMS driver header x86_ems.c plants at INT 67h's segment is NOT
 * linked in, so a guest that hunts EMMXXXX0 by walking this chain rather than
 * by the INT 67h vector will not find it. No title has done that yet; linking
 * it is a change to x86_ems.c, not here. Strategy and interrupt entries are 0,
 * as DOSBox-X leaves them (dos_classes.cpp), because there is no driver to
 * call: a guest that FARs to one gets a fault rather than a wrong answer.
 *
 * WHICH OFFSETS DOES THE GUEST ACTUALLY READ? Worth more than implementing all
 * of them, and NOT cheap: the block is ordinary guest memory reached through
 * the fold table, so seeing a read means trapping the granule, and the pager
 * has no read-only state to express that with -- the seam machinery is a
 * write-side and code-side mechanism. What IS cheap is the call count, so that
 * is what x86_dos_sysvars_calls is; if a title needs the per-offset answer the
 * way to get it is to place the block on its own granule and fault it, which is
 * a pager change and a separate piece of work.
 */
#define SYSVARS_SEG   0x0050u   /* linear 0x500: DOS's own scratch area, above
                                   the BIOS data area and below the HLE stubs */
#define SYSVARS_OFF   0x0022u   /* ES:BX points HERE; -0x22 is the block start,
                                   the same shape DOSBox-X uses so that the
                                   negative offsets have somewhere to live */
#define SYSVARS_BYTES 0x0070u   /* 0x500..0x56F; the DOS 5 block ends at +0x47 */

/* Both ends are compile-time constants, so this is a compile-time check rather
   than something a guest discovers. X86_DOS_STUB_SEG is where x86_hle.c puts
   the vector stubs; running into them would make an interrupt execute SysVars. */
_Static_assert((SYSVARS_SEG << 4) + SYSVARS_BYTES <= (X86_DOS_STUB_SEG << 4),
               "the SysVars block must sit below the HLE stub block");

volatile uint32_t x86_dos_sysvars_calls;
volatile uint32_t x86_dos_parse_calls;

/* Defined with the rest of the INT 21h layer below; used here because this
   block reads as one piece next to the reference it is built from. */
static void ok_clear_cf(void);
static void fail(uint16_t code);
/* The XMS pool's size, defined in the XMS section at the bottom of this file.
   It is the honest answer for "extended memory, KB" and the only one we have. */
extern volatile uint32_t x86_xms_total_kb;

static void sv_w8(uint8_t *p, unsigned off, uint8_t v) { p[off] = v; }

static void sv_w16(uint8_t *p, unsigned off, uint16_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
}

static void sv_wfar(uint8_t *p, unsigned off, uint16_t seg, uint16_t o)
{
    sv_w16(p, off, o);
    sv_w16(p, off + 2, seg);
}

/*
 * Build the block and return ES:BX in the guest. Built on every call rather
 * than once at init: firstMCB and the extended-memory size are live values, and
 * a block stamped at boot would describe an arena that has since moved. DOSBox-X
 * refreshes its block-device count the same way (src/dos/dos.cpp:2433-2436).
 */
static int dos_sysvars(void)
{
    uint8_t *p = x86_guest_ptr(SEG(SYSVARS_SEG), SYSVARS_BYTES);
    if (!p) {
        /* Low memory unbacked. Refusing is right: an ES:BX we did not write is
           the exact failure this function exists to remove. */
        fail(0x01);
        return 0;
    }
    x86_dos_sysvars_calls++;

    memset(p, 0, SYSVARS_BYTES);
    unsigned b = SYSVARS_OFF;                   /* the returned BX, as an index */

    /* -2: the first MCB. The one field every extender reads, and the one we can
       answer without inventing anything -- x86_dos_arena_init() wrote a real
       'Z' header at exactly this segment. */
    sv_w16(p, b - 2, ARENA_FIRST_MCB);

    /* The chains we do not model. 0xFFFF is DOS's end-of-chain offset. */
    sv_wfar(p, b + 0x00, 0xFFFF, 0xFFFF);       /* DPB chain    */
    sv_wfar(p, b + 0x04, 0xFFFF, 0xFFFF);       /* SFT chain    */
    sv_wfar(p, b + 0x08, 0xFFFF, 0xFFFF);       /* CLOCK$       */
    sv_wfar(p, b + 0x0C, 0xFFFF, 0xFFFF);       /* CON          */
    sv_w16 (p, b + 0x10, 512);                  /* max sector size -- REAL */
    sv_wfar(p, b + 0x12, 0xFFFF, 0xFFFF);       /* disk buffers */
    sv_wfar(p, b + 0x16, 0xFFFF, 0xFFFF);       /* CDS array    */
    sv_wfar(p, b + 0x1A, 0xFFFF, 0xFFFF);       /* FCB table    */
    sv_w16 (p, b + 0x1E, 0);                    /* protected FCBs */
    sv_w8  (p, b + 0x20, 1);                    /* block devices  -- REAL */
    sv_w8  (p, b + 0x21, 1);                    /* LASTDRIVE      -- REAL */

    /* The NUL device header. A real header, and a chain that really does end. */
    sv_wfar(p, b + 0x22, 0xFFFF, 0xFFFF);       /* next driver: none */
    sv_w16 (p, b + 0x26, 0x8004);               /* DEVTYP OR ISNULL  */
    sv_w16 (p, b + 0x28, 0);                    /* strategy: no driver */
    sv_w16 (p, b + 0x2A, 0);                    /* interrupt: no driver */
    memcpy(p + b + 0x2C, "NUL     ", 8);

    sv_w8  (p, b + 0x34, 0);                    /* no JOINed drives */
    sv_w16 (p, b + 0x35, 0);                    /* no special-entry segment */
    sv_wfar(p, b + 0x37, 0, 0);                 /* no SETVER list */
    sv_w16 (p, b + 0x3B, 0);                    /* no A20 fix routine */
    sv_w16 (p, b + 0x3D, 0);                    /* nothing in the HMA */
    sv_w16 (p, b + 0x3F, 0);                    /* BUFFERS: we have none, and */
    sv_w16 (p, b + 0x41, 0);                    /*   saying 50 would be filler */
    sv_w8  (p, b + 0x43, 1);                    /* boot drive A:  -- REAL */
    sv_w8  (p, b + 0x44, 1);                    /* 386 DWORD move -- REAL */
    sv_w16 (p, b + 0x45, (uint16_t)x86_xms_total_kb);  /* extended KB -- REAL */

    cpu_state_ptr->segs[0].sel  = SYSVARS_SEG;  /* ES */
    cpu_state_ptr->segs[0].base = SEG(SYSVARS_SEG);
    x86_set_bx(SYSVARS_OFF);
    ok_clear_cf();
    return 1;
}

/* --------------------------------------------------------------------------
 * AH=29h -- PARSE FILENAME INTO FCB.
 *
 * DS:SI is a command line, ES:DI an FCB, AL four parse-control bits. On return
 * AL is 0 (no wildcards), 1 (a `*` or `?` reached a field) or 0xFF (bad drive),
 * and SI has advanced. Dark Forces' extender issues two of these before the far
 * jump that lands in unwritten memory, and a refusal leaves SI where it was --
 * so a caller that parses argument after argument off one line never moves.
 *
 * SOURCE OF RECORD. references/MS-DOS/v4.0/src/DOS/PARSE.ASM:53-61 is the whole
 * of function 29h: it calls MakeFcb and writes the returned SI back into the
 * caller's frame. The algorithm is MakeFcb,
 * references/MS-DOS/v4.0/src/DOS/FCB.ASM:53-230 (v2.0's copy,
 * references/MS-DOS/v2.0/source/FCB.ASM:30-165, is the same code). The
 * character classes are the CharType table at v4.0 FCB.ASM:283-333, whose flags
 * are declared in references/MS-DOS/v4.0/src/INC/DOSSYM.INC:145-148; v2.0's
 * compare chain at FCB.ASM:422-476 agrees set for set. DOSBox-X's
 * FCB_Parsename (references/dosbox-x/src/dos/dos_files.cpp:1641-1785, served at
 * src/dos/dos.cpp:1510-1518) is the cross-check.
 *
 * WHERE THE TWO REFERENCES DISAGREE, AND WHICH ONE THIS FOLLOWS -- MS-DOS,
 * except where our own file layer has already decided otherwise:
 *
 *  - SEPARATOR SET (bit 0). MS-DOS's DELIM is `: < | > + = ; ,` plus tab and
 *    space (FCB.ASM:290-297,307,331); DOSBox-X's FCB_SEP is only `: ; , = +`
 *    (dos_files.cpp:1623) and will not skip a leading `<`, `>` or `|`.
 *    FOLLOWED: MS-DOS.
 *  - WILDCARD FLAG PAST THE FIELD. MS-DOS's `OR DL,1` runs for a `?` at ANY
 *    position, including one consumed and discarded past the 8th character
 *    (FCB.ASM:168-178); DOSBox-X gates it on index < 8 (dos_files.cpp:1733), so
 *    "ABCDEFGHI?.TXT" is AL=1 on MS-DOS and AL=0 on DOSBox-X. FOLLOWED: MS-DOS.
 *  - CONSUMPTION AFTER A BAD DRIVE CHARACTER. DOSBox-X consumes two characters
 *    and abandons the parse (dos_files.cpp:1691, carrying its own `//TODO check`);
 *    MS-DOS never reaches the colon test and consumes NOTHING (FCB.ASM:94-95).
 *    FOLLOWED: MS-DOS.
 *  - WHICH DRIVE LETTERS ARE VALID. MS-DOS range-checks against NUMIO
 *    (v2.0 FCB.ASM:75-80), which for this machine is 1 -- so "C:FOO" would come
 *    back AL=0xFF. FOLLOWED: NEITHER, DELIBERATELY. This port's path layer
 *    already strips ANY drive letter and resolves every one of them to the one
 *    mounted volume (see dos_path_from_guest() above), so failing a letter here
 *    that AH=3Dh will happily open is an inconsistency inside our own DOS, and
 *    the more expensive of the two. Any alphabetic letter parses; 0xFF is
 *    reserved for a non-alphabetic one, which is what `SUB AL,'@'` rejects.
 *    NUMIO in the SysVars block above stays 1, because one block device is the
 *    truth; the letter tolerance is a path-layer accommodation, not a claim
 *    about the hardware.
 *
 * THE OVER-LONG-NAME BEHAVIOUR IS DELIBERATE AND BOTH REFERENCES HAVE IT.
 * `JCXZ MUSTGETWORD` (FCB.ASM:168) keeps consuming characters after the field
 * is full instead of stopping, so "AVERYLONGNAME.TXT" leaves SI past the name
 * and still parses ".TXT". MS-DOS's own comment (FCB.ASM:151-154) calls it a
 * horrible bug. Programs depend on it; it is reproduced, not fixed.
 */

/* The three character classes, spelled out rather than derived, because the
   sets ARE the interface and a clever predicate hides which one it is. */
static int pf_is_space(uint8_t c)   { return c == ' ' || c == '\t'; }

/* DELIM: what one leading separator may be, with bit 0 set. FCB.ASM:290-297. */
static int pf_is_delim(uint8_t c)
{
    return pf_is_space(c) || c == ':' || c == '<' || c == '|' || c == '>' ||
           c == '+' || c == '=' || c == ';' || c == ',';
}

/* What ENDS a name or extension field: every DELIM, plus the path and quoting
   characters, plus every control character. FCB.ASM:284-332. */
static int pf_is_term(uint8_t c)
{
    return c < 0x20u || pf_is_delim(c) || c == '.' || c == '"' || c == '/' ||
           c == '\\' || c == '[' || c == ']';
}

static uint8_t pf_upcase(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
}

/* One byte of the command line, or a terminator if the guest page is not there.
   Reporting an unbacked byte as end-of-input is right: it stops the parse where
   the readable text stops instead of walking into whatever follows. */
static uint8_t pf_peek(uint32_t base, uint16_t si)
{
    uint8_t *p = x86_guest_ptr(base + si, 1);
    return p ? *p : 0;
}

/*
 * MUSTGETWORD (FCB.ASM:155-211): fill one field, then pad it with blanks.
 * `si` is advanced in place; `flag` accumulates the wildcard bit.
 */
static void pf_getword(uint32_t base, uint16_t *si, uint8_t *dst, unsigned n,
                       uint8_t *flag)
{
    unsigned i = 0;
    for (;;) {
        uint8_t c = pf_upcase(pf_peek(base, *si));
        if (pf_is_term(c))
            break;
        (*si)++;
        if (c == '*') {
            /* REP STOSB over the whole remainder, and keep consuming: the star
               eats the rest of the field and everything after it. */
            while (i < n)
                dst[i++] = '?';
            *flag |= 1u;
            continue;
        }
        if (c == '?')
            *flag |= 1u;                /* set even past the field -- see above */
        if (i < n)
            dst[i++] = c;
    }
    while (i < n)                       /* FILLNAM */
        dst[i++] = ' ';
}

static int dos_parse_filename(void)
{
    uint32_t *r = cpu_state_ptr->regs;
    uint8_t al = (uint8_t)r[0];
    uint32_t src_base = cpu_state_ptr->segs[3].base;     /* DS */
    uint32_t fcb_lin = cpu_state_ptr->segs[0].base + (uint16_t)r[7];  /* ES:DI */
    uint16_t si = (uint16_t)r[6];
    uint8_t fcb[16];

    /* The FCB is 16 bytes and DI is wherever the caller put it, so it can
       straddle a granule -- x86_guest_ptr() would refuse the span. Read, edit,
       write back; the copy helpers walk granules. A short read is a genuinely
       unbacked FCB and there is nothing honest to write into it. */
    if (x86_guest_copy_out(fcb, fcb_lin, sizeof(fcb)) != sizeof(fcb)) {
        fail(0x01);
        return 0;
    }

    /* Field defaults. Bits 1/2/3 mean "leave what is already there". */
    if (!(al & 0x02u))
        fcb[0] = 0;                     /* default drive */
    if (!(al & 0x04u))
        memset(fcb + 1, ' ', 8);
    if (!(al & 0x08u))
        memset(fcb + 9, ' ', 3);
    /* The undocumented pair MakeFcb always zeroes (FCB.ASM:81-84): cur_block
       and rec_size. DOSBox-X does the same in ClearBlockRecsize()
       (references/dosbox-x/src/dos/dos_classes.cpp:639-642) and labels it
       "Undocumented bonus work" -- so both references agree it is not optional. */
    memset(fcb + 12, 0, 4);

    /* Bit 0: strip blanks, then AT MOST ONE separator (FCB.ASM:85-90). */
    if (al & 0x01u) {
        while (pf_is_space(pf_peek(src_base, si)))
            si++;
        if (pf_is_delim(pf_peek(src_base, si)))
            si++;
    }
    /* SKPSPC: blanks are stripped again either way (FCB.ASM:91-92). */
    while (pf_is_space(pf_peek(src_base, si)))
        si++;

    uint8_t flag = 0;

    /* Drive. Only a non-terminator followed by ':' is one, and only then is
       anything consumed -- FCB.ASM:94-117 and the NODRV path at :118-120. */
    uint8_t c0 = pf_upcase(pf_peek(src_base, si));
    if (!pf_is_term(c0) && pf_peek(src_base, (uint16_t)(si + 1u)) == ':') {
        uint8_t d = (uint8_t)(c0 - '@');            /* 'A' -> 1 */
        if (d < 1u || d > 26u)
            flag = 0xFFu;                           /* BADDRV */
        fcb[0] = d;                 /* stored either way, as MakeFcb does */
        si = (uint16_t)(si + 2u);
    }

    /* GETWORD (FCB.ASM:147-150): a field that starts on a terminator is left
       exactly as the defaults above left it, and nothing is consumed. */
    if (!pf_is_term(pf_upcase(pf_peek(src_base, si))))
        pf_getword(src_base, &si, fcb + 1, 8, &flag);

    /* The extension is taken only after a literal '.' (FCB.ASM:126-137), and
       MUSTGETWORD rather than GETWORD: "FOO." blanks the extension. */
    if (pf_peek(src_base, si) == '.') {
        si++;
        pf_getword(src_base, &si, fcb + 9, 3, &flag);
    }

    if (x86_guest_copy_in(fcb_lin, fcb, sizeof(fcb)) != sizeof(fcb)) {
        fail(0x01);
        return 0;
    }

    x86_dos_parse_calls++;
    r[6] = (r[6] & 0xFFFF0000u) | si;               /* SI, as PARSE.ASM:57-59 */
    x86_set_al(flag);
    /* DOS does not document CF here and MakeFcb does not touch it; clearing it
       is what a successful call through our HLE must do, since the stub frame
       arrives with whatever the caller pushed. */
    ok_clear_cf();
    return 1;
}

/* --------------------------------------------------------------------------
 * INT 21h
 */

static void ok_clear_cf(void)   { x86_hle_set_cf(0); }
static void fail(uint16_t code) { x86_set_ax(code); x86_hle_set_cf(1); }

/*
 * The file-call trace.
 *
 * It used to be a 32-slot WRAPPING ring of two words. That is the wrong shape
 * for the question it gets asked: a guest stuck in a loop overwrites the whole
 * ring with the loop body in milliseconds, and the call that ISSUED the handle
 * the loop is failing on -- the only interesting entry -- is always gone.
 * WOLF3D's 32 identical AH=42h slots said nothing about where its handle came
 * from, which is the entire question.
 *
 * So it SATURATES at DOS_FILE_LOG_SLOTS instead of wrapping, keeping the
 * PROLOGUE of the run, and `x86_dos_file_calls` counts every call whether it
 * was recorded or not. Writing 0 to `x86_dos_file_log_idx` from the host
 * re-arms it, so a later phase can be captured without a rebuild.
 *
 * Four words per slot, not two: a func/AX pair cannot answer "which handle",
 * and BX plus CX:DX is the difference between "the seek is broken" and "the
 * handle was never issued".
 */
#define DOS_FILE_LOG_SLOTS 96
uint32_t x86_dos_file_log[DOS_FILE_LOG_SLOTS * 4];
uint32_t x86_dos_file_log_idx;
uint32_t x86_dos_file_calls;
char x86_dos_path_log[16][32];
static uint32_t dos_path_log_idx;

/* The MEMORY calls are traced too, but they are not FILE calls: they get a
   slot in the log and are NOT counted in x86_dos_file_calls, whose meaning
   other reports depend on. For these, slot+3 carries (ES << 16) | DX rather
   than CX:DX -- ES is the block for 49h/4Ah and DX is meaningless there, and
   "which block" is the whole question a failing resize loop asks. */
static int dos_func_is_mem(uint8_t f)
{
    return f == 0x48 || f == 0x49 || f == 0x4A;
}

static int dos_func_is_file(uint8_t f)
{
    switch (f) {
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x1A: case 0x21: case 0x22:
    case 0x23: case 0x27: case 0x28: case 0x29: case 0x2F:
    case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E:
    case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x45:
    case 0x4B:                  /* EXEC opens a file, and the path log is the
                                   first thing asked for when one fails */
    case 0x46: case 0x47: case 0x4E: case 0x4F: case 0x56: case 0x57:
    case 0x5A: case 0x5B: case 0x68: case 0x6C:
        return 1;
    default:
        return 0;
    }
}

static void dos_log_path(const char *p)
{
    unsigned slot = dos_path_log_idx & 15;
    strncpy(x86_dos_path_log[slot], p, sizeof(x86_dos_path_log[0]) - 1);
    x86_dos_path_log[slot][sizeof(x86_dos_path_log[0]) - 1] = 0;
    dos_path_log_idx++;
}

static int dos_int21_dispatch(uint8_t func);

int x86_dos_int21(uint8_t func)
{
    int is_mem = dos_func_is_mem(func);
    if (!is_mem && !dos_func_is_file(func))
        return dos_int21_dispatch(func);

    if (!is_mem)
        x86_dos_file_calls++;
    if (x86_dos_file_log_idx >= DOS_FILE_LOG_SLOTS)
        return dos_int21_dispatch(func);

    unsigned slot = x86_dos_file_log_idx * 4;
    x86_dos_file_log[slot] =
        ((uint32_t)func << 8) | (uint8_t)(cpu_state_ptr->regs[0] & 0xFF);
    x86_dos_file_log[slot + 2] = (uint16_t)(cpu_state_ptr->regs[3] & 0xFFFF);
    x86_dos_file_log[slot + 3] = is_mem ?
        (((uint32_t)cpu_state_ptr->segs[0].sel << 16) |
         (uint16_t)(cpu_state_ptr->regs[2] & 0xFFFFu)) :
        (((uint32_t)(cpu_state_ptr->regs[1] & 0xFFFFu) << 16) |
         (uint16_t)(cpu_state_ptr->regs[2] & 0xFFFFu));
    int rc = dos_int21_dispatch(func);
    uint16_t *fl = x86_hle_flags_on_stack();
    x86_dos_file_log[slot + 1] =
        (uint32_t)((fl && (*fl & 1u)) ? 0x10000u : 0u) |
        (uint16_t)(cpu_state_ptr->regs[0] & 0xFFFF);
    x86_dos_file_log_idx++;
    return rc;
}

static int dos_int21_dispatch(uint8_t func)
{
    uint32_t *r = cpu_state_ptr->regs;
    uint16_t dx = (uint16_t)r[2];
    uint16_t cx = (uint16_t)r[1];
    uint16_t bx = (uint16_t)r[3];

    switch (func) {
    case 0x00:
        return x86_dos_terminate_code(0);
    case 0x4C:
        /* AL is the return code, and it is the whole point of AH=4Dh below:
           a parent that EXECs a child reads it to find out whether the child
           worked. Discarding it here made 4Dh unimplementable. */
        return x86_dos_terminate_code((uint16_t)(r[0] & 0xFFu));

    case 0x4B:                                  /* EXEC */
        return dos_exec_child();

#if X86_DOS_EXEC
    case 0x4D:                                  /* get child return code */
        /* AH is the exit TYPE (0 = a normal AH=4Ch), AL the code. DOS returns
           it ONCE and then forgets it, so a second AH=4Dh reports 0 -- a
           parent that polls must not see the same child exit twice. */
        x86_set_ax(dos_ret_code);
        dos_ret_code = 0;
        ok_clear_cf();
        return 1;
#endif

    case 0x02:                                  /* character output */
        x86_video_teletype((uint8_t)dx);
        return 1;

    /*
     * The console family. AH=06h in particular: a program that wants to write
     * without DOS interpreting control characters uses it instead of AH=02h or
     * AH=09h, and TOPBENCH prints its entire report through it. Unanswered, it
     * printed nothing at all and the screen stayed at 80x25 of spaces.
     *
     * DL=0xFF makes AH=06h an INPUT call, and it must NOT block: ZF reports
     * whether a character was there. The genuinely blocking forms return 2,
     * which rewinds the INT so it re-executes on the next slice -- blocking
     * inside the service would stop the frame loop and with it the input that
     * would satisfy it.
     */
    case 0x06: {                                /* direct console I/O */
        uint16_t key;
        if ((dx & 0xFFu) != 0xFFu) {
            x86_video_teletype((uint8_t)dx);
            return 1;
        }
        if (x86_kb_get(&key)) {
            x86_set_al((uint8_t)key);
            x86_hle_set_zf(0);
        } else {
            x86_set_al(0);
            x86_hle_set_zf(1);
        }
        return 1;
    }
    case 0x01:                                  /* input, echoed */
    case 0x07:                                  /* direct input, no echo */
    case 0x08: {                                /* input, no echo */
        uint16_t key;
        if (!x86_kb_get(&key))
            return 2;                           /* nothing yet: retry */
        x86_set_al((uint8_t)key);
        if (func == 0x01)
            x86_video_teletype((uint8_t)key);
        return 1;
    }
    case 0x0B: {                                /* input status */
        uint16_t key;
        x86_set_al(x86_kb_peek(&key) ? 0xFF : 0x00);
        return 1;
    }
    case 0x0C:                                  /* flush, then input */
        return 1;

    case 0x09: {                                /* $-terminated string */
        uint32_t lin = cpu_state_ptr->segs[3].base + dx;
        for (unsigned i = 0; i < 4096; i++) {
            uint8_t *p = x86_guest_ptr(lin + i, 1);
            if (!p || *p == '$')
                break;
            x86_video_teletype(*p);
        }
        return 1;
    }

    case 0x1A:                                  /* set DTA */
        dta_lin = cpu_state_ptr->segs[3].base + dx;
        return 1;
    case 0x2F:                                  /* get DTA */
        /* ES:BX, not BX alone. The old form returned the bottom NIBBLE of the
           linear address and nothing else, so a program that saved the DTA and
           restored it later pointed at ES:000x -- the interrupt vector table
           of whatever segment ES held. */
        x86_set_bx((uint16_t)(dta_lin & 0x0F));
        uint16_t dseg = (uint16_t)((dta_lin >> 4) & 0xFFFFu);
        cpu_state_ptr->segs[0].sel = dseg;      /* ES */
        cpu_state_ptr->segs[0].base = SEG(dseg);
        return 1;

    case 0x25: {                                /* set interrupt vector */
        uint8_t *ivt = x86_guest_ptr((uint32_t)(uint8_t)r[0] * 4u, 4);
        if (ivt) {
            ivt[0] = (uint8_t)dx;
            ivt[1] = (uint8_t)(dx >> 8);
            ivt[2] = (uint8_t)cpu_state_ptr->segs[3].sel;
            ivt[3] = (uint8_t)(cpu_state_ptr->segs[3].sel >> 8);
        }
        return 1;
    }
    case 0x35: {                                /* get interrupt vector */
        uint8_t *ivt = x86_guest_ptr((uint32_t)(uint8_t)r[0] * 4u, 4);
        if (ivt) {
            x86_set_bx((uint16_t)(ivt[0] | (ivt[1] << 8)));
            uint16_t seg = (uint16_t)(ivt[2] | (ivt[3] << 8));
            cpu_state_ptr->segs[0].sel = seg;           /* ES */
            cpu_state_ptr->segs[0].base = SEG(seg);
        }
        return 1;
    }

    case 0x52:                                  /* get list of lists */
        return dos_sysvars();
    case 0x29:                                  /* parse filename into FCB */
        return dos_parse_filename();

    case 0x30:                                  /* DOS version 5.00 */
        x86_set_ax(0x0005);
        x86_set_bx(0);
        x86_set_cx(0);
        return 1;

    case 0x2C:                                  /* get time */
        x86_set_cx(0x0C00);
        x86_set_dx(0);
        return 1;
    case 0x2A:                                  /* get date */
        x86_set_cx(1990);
        x86_set_dx(0x0101);
        return 1;

    /* ---- memory ---------------------------------------------------- */
    case 0x48: {                                /* allocate */
        uint16_t largest = 0;
        uint16_t seg = mcb_alloc(bx, psp_seg, &largest);
        if (seg) {
            x86_set_ax(seg);
            ok_clear_cf();
        } else {
            x86_set_bx(largest);
            fail(0x08);
        }
        return 1;
    }
    case 0x49:                                  /* free */
        if (mcb_free(cpu_state_ptr->segs[0].sel))
            ok_clear_cf();
        else
            fail(0x09);
        return 1;
    case 0x4A: {                                /* resize */
        uint16_t largest = 0;
        if (mcb_resize(cpu_state_ptr->segs[0].sel, bx, &largest)) {
            ok_clear_cf();
        } else {
            x86_set_bx(largest);
            fail(0x08);
        }
        return 1;
    }

    /* ---- files ------------------------------------------------------ */
    /*
     * AH=3Ch, create/truncate. TOPBENCH's full-screen UI opens a stream and
     * dies without one: unanswered, the default arm below reported "invalid
     * function", which Borland's runtime prints as `Runtime error 001` -- and
     * the half-drawn screen behind that message is what looked like a broken
     * renderer.
     *
     * CX carries the file attributes; nothing here has an attribute to store,
     * so it is ignored deliberately rather than by omission.
     */
    case 0x3C: {                                /* create / truncate */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
        dos_log_path(path);
        int dev = dos_name_is_device(path);
        if (dev) {
            x86_set_ax(DOS_DEV_HANDLE(dev));
            ok_clear_cf();
            return 1;
        }
        int slot = handle_alloc();
        if (slot < 0) { fail(0x04); return 1; }
        if (f_open(&dos_files[slot], path,
                   FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK) {
            dos_file_used[slot] = 0;
            fail(0x05);                         /* access denied */
            return 1;
        }
        x86_set_ax((uint16_t)(slot + 5));
        ok_clear_cf();
        return 1;
    }
    case 0x3D: {                                /* open */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
        dos_log_path(path);
        int dev = dos_name_is_device(path);
        if (dev) {
            if (dev == DOS_DEV_EMS)
                x86_ems_device_opens++;
            x86_set_ax(DOS_DEV_HANDLE(dev));
            ok_clear_cf();
            return 1;
        }
        int slot = handle_alloc();
        if (slot < 0) { fail(0x04); return 1; }
        BYTE mode = ((uint8_t)r[0] & 3) == 0 ? FA_READ : FA_READ | FA_WRITE;
        if (f_open(&dos_files[slot], path, mode) != FR_OK) {
            dos_file_used[slot] = 0;
            fail(0x02);
            return 1;
        }
        x86_set_ax((uint16_t)(slot + 5));
        ok_clear_cf();
        return 1;
    }
    case 0x44:                                  /* IOCTL */
        /* AL=00 get device information. A program asks this about its own
           stdout to find out whether it is talking to the console or to a
           redirected file, and TOPBENCH asks it before deciding how to report.
           Unanswered, it read whatever DX happened to hold and went off into
           memory it does not own.
           Handles 0-2 are the console: 0x80D3 is what a real DOS returns for
           CON (character device, is-stdin, is-stdout, special). Anything else
           is one of our FatFs handles, i.e. a file on drive A. */
        switch ((uint8_t)(r[0] & 0xFF)) {
        case 0x00:
            /* EMMXXXX0 is a character device but it is NOT the console: it is
               neither stdin nor stdout, and a program that believed it was
               would write its output into the memory manager. 0x80C0 is a
               plain character device with the EOF and "not a disk file" bits,
               which is what a real EMM's header attribute translates to. */
            if (dos_handle_device(bx) == DOS_DEV_EMS)
                x86_set_dx(0x80C0);
            else if (dos_handle_device(bx))
                x86_set_dx(0x80D3);
            else if (handle_get(bx))
                x86_set_dx(0x0000);             /* file, drive A, not a device */
            else { fail(0x06); return 1; }
            x86_set_ax((uint16_t)(cpu_state_ptr->regs[2] & 0xFFFF));
            ok_clear_cf();
            return 1;
        case 0x01:                              /* set device information */
            ok_clear_cf();
            return 1;
        default:
            fail(0x01);                         /* invalid function */
            return 1;
        }
    case 0x3E: {                                /* close */
        if (dos_handle_device(bx)) { ok_clear_cf(); return 1; }
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        f_close(f);
        dos_file_used[bx - 5] = 0;
        ok_clear_cf();
        return 1;
    }
    case 0x3F: {                                /* read */
        int dev = dos_handle_device(bx);
        if (dev == DOS_DEV_NUL) {               /* always at end of file */
            x86_set_ax(0);
            ok_clear_cf();
            return 1;
        }
        if (dev == DOS_DEV_CON) {
            /* The console, one line at a time. Blocking here would stop the
               frame loop and with it the keyboard that would satisfy the
               read, so an empty queue rewinds the INT instead (return 2). */
            uint32_t lin = cpu_state_ptr->segs[3].base + dx;
            uint16_t key;
            uint32_t n = 0;
            while (n < cx) {
                if (!x86_kb_peek(&key))
                    break;
                x86_kb_get(&key);
                uint8_t c = (uint8_t)key;
                if (c == 0x0D) c = 0x0A;
                x86_video_teletype(c);          /* DOS echoes a console read */
                if (x86_guest_copy_in(lin + n, &c, 1) != 1)
                    break;
                n++;
                if (c == 0x0A)
                    break;
            }
            if (n == 0 && cx)
                return 2;
            x86_set_ax((uint16_t)n);
            ok_clear_cf();
            return 1;
        }
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        uint32_t lin = cpu_state_ptr->segs[3].base + dx;
        uint32_t total = 0;
        while (total < cx) {
            uint8_t buf[512];
            UINT want = (cx - total > sizeof(buf)) ? sizeof(buf)
                                                   : (UINT)(cx - total);
            UINT n = 0;
            if (f_read(f, buf, want, &n) != FR_OK || n == 0)
                break;
            if (x86_guest_copy_in(lin + total, buf, n) != n)
                break;
            total += n;
        }
        x86_set_ax((uint16_t)total);
        ok_clear_cf();
        return 1;
    }
    case 0x40: {                                /* write */
        int dev = dos_handle_device(bx);
        if (dev) {
            uint32_t lin = cpu_state_ptr->segs[3].base + dx;
            if (dev == DOS_DEV_CON) {
                for (uint32_t i = 0; i < cx; i++) {
                    uint8_t *p = x86_guest_ptr(lin + i, 1);
                    if (!p)
                        break;
                    x86_video_teletype(*p);
                }
            }
            x86_set_ax(cx);                     /* a sink accepts everything */
            ok_clear_cf();
            return 1;
        }
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        uint32_t lin = cpu_state_ptr->segs[3].base + dx;
        uint32_t total = 0;
        while (total < cx) {
            uint8_t buf[512];
            uint32_t want = (cx - total > sizeof(buf)) ? sizeof(buf)
                                                       : (cx - total);
            uint32_t got = x86_guest_copy_out(buf, lin + total, want);
            if (!got) break;
            UINT n = 0;
            if (f_write(f, buf, got, &n) != FR_OK || n == 0)
                break;
            total += n;
        }
        x86_set_ax((uint16_t)total);
        ok_clear_cf();
        return 1;
    }
    /*
     * AH=43h, get/set file attributes -- MEASURED as what stopped TOPBENCH.
     * Its file log read 3Ch/40h/3Eh (create, write, close its own log) and then
     * 43h AL=00 on DATABASE.INI, which the default arm answered with CF set and
     * AX=1 "invalid function". A program cannot tell that from "no such file",
     * so it reported its data file as unreadable and never issued the 3Dh that
     * would have opened it. The open path was never reached, let alone broken.
     *
     * FatFs is FF_USE_CHMOD=0, so SET is accepted for a file that exists and
     * changes nothing: refusing it would break a program that merely clears the
     * archive bit, and the attribute has no meaning on a read-back of the .dsk.
     */
    case 0x43: {                                /* get / set file attributes */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        dos_log_path(path);
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);
        if (dos_name_is_device(path)) {         /* devices have no directory */
            x86_set_cx(0x0040);                 /* AM_ARC-ish: "just a file" */
            ok_clear_cf();
            return 1;
        }
        FILINFO fno;
        if (f_stat(path, &fno) != FR_OK) { fail(0x02); return 1; }
        if (((uint8_t)r[0] & 0xFF) == 0x00)
            x86_set_cx(fno.fattrib);
        ok_clear_cf();
        return 1;
    }

    case 0x41: {                                /* delete file */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        dos_log_path(path);
        if (f_unlink(path) != FR_OK) { fail(0x02); return 1; }
        ok_clear_cf();
        return 1;
    }
    case 0x56: {                                /* rename */
        char from[64], to[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, from,
                            sizeof(from));
        dos_path_from_guest(cpu_state_ptr->segs[0].base +
                            (uint16_t)cpu_state_ptr->regs[7], to, sizeof(to));
        if (f_rename(from, to) != FR_OK) { fail(0x02); return 1; }
        ok_clear_cf();
        return 1;
    }

    /*
     * FindFirst / FindNext. Old tools reach their data files through these as
     * often as through 3Dh -- a wildcard is how a program discovers what is on
     * the disk at all -- and unanswered they look exactly like an empty floppy.
     */
    case 0x4E: {                                /* find first */
        char path[64];
        dos_path_from_guest(cpu_state_ptr->segs[3].base + dx, path,
                            sizeof(path));
        dos_log_path(path);
        strncpy(x86_dos_last_path, path, sizeof(x86_dos_last_path) - 1);

        /* Split the directory off the pattern. One volume, root only. */
        char *stem = path;
        for (char *q = path; *q; q++)
            if (*q == '\\' || *q == '/')
                stem = q + 1;

        if (dos_find_open) { f_closedir(&dos_find_dir); dos_find_open = 0; }
        if (f_opendir(&dos_find_dir, "") != FR_OK) { fail(0x03); return 1; }
        dos_find_open = 1;
        dos_find_serial++;
        dos_find_attr = (uint8_t)(cx & (AM_HID | AM_SYS | AM_DIR));
        dos_83_pattern(stem, dos_find_pat);
        if (!dos_find_step()) {
            f_closedir(&dos_find_dir);
            dos_find_open = 0;
            fail(0x12);                         /* no more files */
            return 1;
        }
        x86_set_ax(0);
        ok_clear_cf();
        return 1;
    }
    case 0x4F: {                                /* find next */
        uint8_t d[2];
        if (!dos_find_open ||
            x86_guest_copy_out(d, dta_lin, 2) != 2 ||
            d[0] != DOS_FIND_MAGIC || d[1] != dos_find_serial) {
            fail(0x12);
            return 1;
        }
        if (!dos_find_step()) {
            f_closedir(&dos_find_dir);
            dos_find_open = 0;
            fail(0x12);
            return 1;
        }
        x86_set_ax(0);
        ok_clear_cf();
        return 1;
    }

    case 0x19:                                  /* current drive */
        x86_set_al(0);                          /* A: -- the only one there is */
        return 1;
    case 0x0E:                                  /* select drive */
        x86_set_al(1);                          /* one drive in the system */
        return 1;
    case 0x47: {                                /* get current directory */
        /*
         * The CWD, in DOS notation and WITHOUT a leading backslash -- which is
         * what the DOS API specifies and what a caller re-joins with one.
         *
         * "Root, always" was not a free answer. DOS/4GW's stub asks here and
         * then opens `<cwd>\DOS4GW.EXE` as an ABSOLUTE path; told the root it
         * opened `\DOS4GW.EXE`, got file-not-found for a file that is in \DF,
         * printed three lines and exited with errorlevel 1.
         */
        uint32_t lin = cpu_state_ptr->segs[3].base + (uint16_t)r[6];
        const char *c = x86_dos_get_cwd();
        char buf[64];
        unsigned i = 0;
        while (c[i] && i < sizeof(buf) - 1) {
            buf[i] = (c[i] == '/') ? '\\' : c[i];
            i++;
        }
        buf[i] = 0;
        x86_guest_copy_in(lin, buf, i + 1);
        x86_set_ax(0x0100);
        ok_clear_cf();
        return 1;
    }

    case 0x42: {                                /* seek */
        /* A character device has no file pointer, and DOS says so by
           SUCCEEDING with DX:AX = 0 rather than by failing. 3Eh/3Fh/40h/44h
           all consult dos_handle_device(); 42h was the one that did not, so a
           seek on handle 0/1/2 came back "invalid handle" -- and a program
           that treats that as retryable sits in the retry forever. WOLF3D
           issued 81 of them in a row before walking off into 0xFF filler. */
        if (dos_handle_device(bx)) {
            x86_set_ax(0);
            x86_set_dx(0);
            ok_clear_cf();
            return 1;
        }
        FIL *f = handle_get(bx);
        if (!f) { fail(0x06); return 1; }
        uint32_t off = ((uint32_t)cx << 16) | dx;
        uint8_t whence = (uint8_t)r[0];
        FSIZE_t base = (whence == 1) ? f_tell(f)
                     : (whence == 2) ? f_size(f) : 0;
        FSIZE_t pos = base + (FSIZE_t)off;
        if (f_lseek(f, pos) != FR_OK) { fail(0x19); return 1; }
        x86_set_ax((uint16_t)pos);
        x86_set_dx((uint16_t)(pos >> 16));
        ok_clear_cf();
        return 1;
    }

    default:
        x86_hle_unhandled_vector = 0x21;
        x86_hle_unhandled_func = func;
        x86_dos_last_error = func;
        fail(0x01);                             /* invalid function */
        return 0;
    }
}

/* ===========================================================================
 * XMS 2.0 -- the HIMEM interface. Rationale, the storage model and the two
 * things we refuse are all in x86_xms.h; this file is the mechanism.
 *
 * It lives in x86_dos.c rather than a file of its own because it is the same
 * kind of thing as the MCB arena above -- a memory manager the guest talks to
 * through an HLE entry -- and because a new translation unit is a Makefile
 * change this branch has no other reason to make.
 * ===========================================================================
 */
#include "x86_xms.h"

volatile uint32_t x86_xms_total_kb;
volatile uint32_t x86_xms_used_kb;
volatile uint32_t x86_xms_handles_used;
volatile uint32_t x86_xms_detects;
volatile uint32_t x86_xms_entry_reqs;
volatile uint32_t x86_xms_calls;
volatile uint32_t x86_xms_moves;
volatile uint32_t x86_xms_move_up;
volatile uint32_t x86_xms_move_down;
volatile uint32_t x86_xms_move_xx;
volatile uint32_t x86_xms_io_errors;
volatile uint32_t x86_xms_guest_errors;
volatile uint32_t x86_xms_last_err;
volatile uint32_t x86_xms_unhandled;

/* Error codes, spelled out because the numbers are the interface. */
#define XMSE_NOFUNC     0x80u   /* function not implemented                 */
#define XMSE_NOHMA      0x90u   /* the HMA does not exist                   */
#define XMSE_NOTALLOC   0x93u   /* HMA not allocated                        */
#define XMSE_NOMEM      0xA0u   /* all extended memory is allocated         */
#define XMSE_NOHANDLE   0xA1u   /* all handles are in use                   */
#define XMSE_BADHANDLE  0xA2u
#define XMSE_BADSRCH    0xA3u
#define XMSE_BADSRCO    0xA4u
#define XMSE_BADDSTH    0xA5u
#define XMSE_BADDSTO    0xA6u
#define XMSE_BADLEN     0xA7u
#define XMSE_OVERLAP    0xA8u
#define XMSE_PARITY     0xA9u   /* what a driver returns for a store failure */
#define XMSE_NOTLOCKED  0xAAu
#define XMSE_LOCKED     0xABu
#define XMSE_LOCKFAIL   0xADu

typedef struct {
    uint8_t  used;
    uint8_t  locks;
    uint32_t kb_off;            /* offset into the store, in KB  */
    uint32_t kb;                /* size, in KB                   */
} xms_handle_t;

static xms_handle_t xms_h[X86_XMS_HANDLES];
static x86_xms_open_fn  xms_open;
static x86_xms_close_fn xms_close;
static x86_xms_rd_fn    xms_rd;
static x86_xms_wr_fn    xms_wr;
static uint32_t         xms_kb;         /* the whole store, in KB */
static uint32_t         xms_a20_local;  /* AH=05h/06h nesting count */

/* The ONLY buffer this interface owns, and it is 256 bytes because the two
   paths that need one are rare: an XMS->XMS move and the copy inside a growing
   AH=0Fh. Guest<->XMS moves need no buffer at all -- x86_guest_ptr() hands out
   a host pointer for one page-run and the store reads or writes straight
   through it, so the common path is zero-copy. */
#define XMS_CHUNK 256u
static uint8_t xms_buf[XMS_CHUNK];

void x86_xms_set_backing(x86_xms_open_fn op, x86_xms_close_fn cl,
                         x86_xms_rd_fn rd, x86_xms_wr_fn wr, uint32_t kb)
{
    xms_open = op;
    xms_close = cl;
    xms_rd = rd;
    xms_wr = wr;
    /* No store means no memory, and we say so rather than handing out bytes we
       cannot keep. See x86_xms.h. */
    xms_kb = (rd && wr) ? kb : 0u;
    x86_xms_reset();
}

void x86_xms_reset(void)
{
    for (unsigned i = 0; i < X86_XMS_HANDLES; i++) {
        xms_h[i].used = 0;
        xms_h[i].locks = 0;
        xms_h[i].kb_off = 0;
        xms_h[i].kb = 0;
    }
    xms_a20_local = 0;
    x86_xms_total_kb = xms_kb;
    x86_xms_used_kb = 0;
    x86_xms_handles_used = 0;
}

/* ---- the allocator -------------------------------------------------------
 * KB units throughout, because the interface is in KB and any other unit needs
 * a rounding rule that AH=08h and AH=0Eh would then have to agree on. First
 * fit over the live blocks: sixteen handles, so walking them is cheaper than
 * any structure that would keep them sorted.
 */
#define XMS_NO_OFF 0xFFFFFFFFu

static uint32_t xms_find_gap(uint32_t kb)
{
    uint32_t cand = 0;
    for (;;) {
        if (cand > xms_kb || xms_kb - cand < kb)
            return XMS_NO_OFF;
        uint32_t bump = 0;
        for (unsigned i = 0; i < X86_XMS_HANDLES; i++) {
            if (!xms_h[i].used)
                continue;
            uint32_t s = xms_h[i].kb_off, e = s + xms_h[i].kb;
            if (cand < e && s < cand + kb) {
                if (e > bump)
                    bump = e;
            }
        }
        if (!bump)
            return cand;
        cand = bump;
    }
}

/* The largest block AH=08h may promise. Not "total free" -- a guest that asks
   for the total and gets a fragmented refusal is the failure mode AX and DX
   are two separate registers to prevent. */
static uint32_t xms_largest_free(void)
{
    uint32_t best = 0;
    /* Every gap starts either at 0 or at the end of some live block. */
    for (unsigned i = 0; i <= X86_XMS_HANDLES; i++) {
        uint32_t start = 0;
        if (i < X86_XMS_HANDLES) {
            if (!xms_h[i].used)
                continue;
            start = xms_h[i].kb_off + xms_h[i].kb;
        }
        if (start >= xms_kb)
            continue;
        uint32_t end = xms_kb;
        for (unsigned j = 0; j < X86_XMS_HANDLES; j++) {
            if (!xms_h[j].used)
                continue;
            uint32_t s = xms_h[j].kb_off;
            if (s >= start && s < end)
                end = s;
        }
        if (end - start > best)
            best = end - start;
    }
    return best;
}

static uint32_t xms_free_kb(void)
{
    uint32_t used = 0;
    for (unsigned i = 0; i < X86_XMS_HANDLES; i++)
        if (xms_h[i].used)
            used += xms_h[i].kb;
    return xms_kb - used;
}

static void xms_account(void)
{
    uint32_t used = 0, n = 0;
    for (unsigned i = 0; i < X86_XMS_HANDLES; i++)
        if (xms_h[i].used) { used += xms_h[i].kb; n++; }
    x86_xms_used_kb = used;
    x86_xms_handles_used = n;
    x86_xms_total_kb = xms_kb;
}

static xms_handle_t *xms_get(uint16_t handle)
{
    if (handle == 0 || handle > X86_XMS_HANDLES)
        return NULL;
    xms_handle_t *h = &xms_h[handle - 1];
    return h->used ? h : NULL;
}

/* ---- results -------------------------------------------------------------
 * AX=1/BL=0 for success, AX=0/BL=code for failure. There is NO CF here: the
 * control function is entered by CALL FAR, so there is no FLAGS word on the
 * stack to edit (x86_xms.h). Touching one would corrupt the caller's stack.
 */
static void xms_bl_set(uint8_t v)
{
    cpu_state_ptr->regs[3] = (cpu_state_ptr->regs[3] & 0xFFFFFF00u) | v;
}
static void xms_bh_set(uint8_t v)
{
    cpu_state_ptr->regs[3] = (cpu_state_ptr->regs[3] & 0xFFFF00FFu)
                             | ((uint32_t)v << 8);
}
static int xms_ok(void)   { x86_set_ax(1); xms_bl_set(0); return 1; }
static int xms_err(uint8_t code)
{
    x86_set_ax(0);
    xms_bl_set(code);
    x86_xms_last_err = code;
    return 1;
}

/* ---- the store -----------------------------------------------------------
 * One open per OPERATION, not per page-run: the pagefile is open-per-call and
 * ~25 ms of that is the open (docs/12 §1a), so bracketing the whole move is
 * the difference between one open and twenty-six for a 102 KB transfer.
 */
static int xms_store_begin(int write)
{
    if (!xms_rd || !xms_wr)
        return -1;
    if (xms_open && xms_open(write) != 0) {
        x86_xms_io_errors++;
        return -1;
    }
    return 0;
}
static void xms_store_end(void)
{
    if (xms_close)
        xms_close();
}

/*
 * Move one contiguous run between the store and GUEST memory.
 *
 * PAGE-RUN AT A TIME, and that is not an optimisation, it is the granule seam
 * (CLAUDE.md, docs/10). `host = lin + k` with one k per 4 KB granule and frames
 * handed out in FAULT order, so guest page N+1 is almost never the host memory
 * after page N -- one memcpy over `x86_guest_ptr(lin, bytes)` for a range that
 * spans a boundary runs off the top of a frame into an unrelated page. That is
 * the bug shape that made the exec fallback execute the IVT. x86_guest_ptr()
 * refuses a spanning request, so this splits at every boundary itself.
 */
static int xms_guest_run(uint32_t lin, uint32_t off, uint32_t bytes, int to_xms)
{
    while (bytes) {
        uint32_t page_off = lin & (X86_PG_SIZE - 1u);
        uint32_t run = X86_PG_SIZE - page_off;
        if (run > bytes)
            run = bytes;
        void *p = x86_guest_ptr(lin, run);
        if (!p) {
            x86_xms_guest_errors++;
            return -1;
        }
        int rc = to_xms ? xms_wr(off, p, run) : xms_rd(off, p, run);
        if (rc != 0) {
            x86_xms_io_errors++;
            return -1;
        }
        lin += run;
        off += run;
        bytes -= run;
    }
    return 0;
}

/* Store to store. The only path that needs the buffer, and the only one where
   overlap is possible -- the spec allows a move within one handle, so an
   overlapping one is copied backwards rather than refused. */
static int xms_store_run(uint32_t dst, uint32_t src, uint32_t bytes)
{
    int back = (dst > src && dst < src + bytes);
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t run = bytes - done;
        if (run > XMS_CHUNK)
            run = XMS_CHUNK;
        uint32_t s = back ? src + bytes - done - run : src + done;
        uint32_t d = back ? dst + bytes - done - run : dst + done;
        if (xms_rd(s, xms_buf, run) != 0 || xms_wr(d, xms_buf, run) != 0) {
            x86_xms_io_errors++;
            return -1;
        }
        done += run;
    }
    return 0;
}

/* ---- AH=0Bh --------------------------------------------------------------
 * DS:SI points at the move structure.
 *
 *   +0  dd  length in BYTES, must be even
 *   +4  dw  source handle, 0 = the source is a real-mode SEG:OFF
 *   +6  dd  source offset -- into the block, or (seg << 16) | off
 *   +10 dw  destination handle
 *   +12 dd  destination offset
 */
static int xms_move(void)
{
    uint8_t st[16];
    uint32_t lin = cpu_state_ptr->segs[3].base
                   + (cpu_state_ptr->regs[6] & 0xFFFFu);

    if (x86_guest_copy_out(st, lin, sizeof(st)) != sizeof(st)) {
        x86_xms_guest_errors++;
        return xms_err(XMSE_PARITY);
    }
    uint32_t len = (uint32_t)st[0] | ((uint32_t)st[1] << 8)
                 | ((uint32_t)st[2] << 16) | ((uint32_t)st[3] << 24);
    uint16_t sh  = (uint16_t)(st[4] | (st[5] << 8));
    uint32_t so  = (uint32_t)st[6] | ((uint32_t)st[7] << 8)
                 | ((uint32_t)st[8] << 16) | ((uint32_t)st[9] << 24);
    uint16_t dh  = (uint16_t)(st[10] | (st[11] << 8));
    uint32_t dof = (uint32_t)st[12] | ((uint32_t)st[13] << 8)
                 | ((uint32_t)st[14] << 16) | ((uint32_t)st[15] << 24);

    if (len & 1u)
        return xms_err(XMSE_BADLEN);
    if (len == 0)
        return xms_ok();                    /* a legal no-op */

    /* Resolve both ends. A handle end is bounds-checked against the block; a
       conventional end is a seg:off that x86_guest_ptr() validates a run at a
       time, so a bad one fails as a guest error and not as a wild write. */
    uint32_t s_lin = 0, d_lin = 0, s_off = 0, d_off = 0;
    if (sh) {
        xms_handle_t *h = xms_get(sh);
        if (!h)
            return xms_err(XMSE_BADSRCH);
        if (so > (h->kb << 10) || (h->kb << 10) - so < len)
            return xms_err(XMSE_BADSRCO);
        s_off = (h->kb_off << 10) + so;
    } else {
        s_lin = ((so >> 16) << 4) + (so & 0xFFFFu);
    }
    if (dh) {
        xms_handle_t *h = xms_get(dh);
        if (!h)
            return xms_err(XMSE_BADDSTH);
        if (dof > (h->kb << 10) || (h->kb << 10) - dof < len)
            return xms_err(XMSE_BADDSTO);
        d_off = (h->kb_off << 10) + dof;
    } else {
        d_lin = ((dof >> 16) << 4) + (dof & 0xFFFFu);
    }
    if (!sh && !dh) {
        /* Both ends conventional. The spec permits it; nothing in this project
           needs it, and doing it wrong is worse than saying no. */
        x86_xms_unhandled = 0x0B;
        return xms_err(XMSE_OVERLAP);
    }

    if (xms_store_begin(1) != 0)
        return xms_err(XMSE_PARITY);
    int rc;
    if (sh && dh) {
        rc = xms_store_run(d_off, s_off, len);
        if (rc == 0) x86_xms_move_xx += len;
    } else if (dh) {
        rc = xms_guest_run(s_lin, d_off, len, 1);
        if (rc == 0) x86_xms_move_up += len;
    } else {
        rc = xms_guest_run(d_lin, s_off, len, 0);
        if (rc == 0) x86_xms_move_down += len;
    }
    xms_store_end();
    if (rc != 0)
        return xms_err(XMSE_PARITY);
    x86_xms_moves++;
    return xms_ok();
}

/* ---- the control function ------------------------------------------------ */
int x86_xms_control(void)
{
    uint8_t func = (uint8_t)(cpu_state_ptr->regs[0] >> 8);
    uint16_t dx = (uint16_t)cpu_state_ptr->regs[2];
    uint16_t bx = (uint16_t)cpu_state_ptr->regs[3];

    x86_xms_calls++;
    switch (func) {
    case 0x00:                                  /* get version */
        /* 2.00, and the function set below IS the 2.0 set. Claiming 3.0 would
           advertise the super-extended calls (88h/89h/8Eh/8Fh) we do not have,
           and a guest picks its path from this number. */
        x86_set_ax(0x0200);
        x86_set_bx(0x0001);                     /* internal revision */
        x86_set_dx(0x0000);                     /* no HMA -- see x86_xms.h */
        return 1;

    case 0x01:                                  /* request the HMA */
    case 0x02:                                  /* release the HMA */
        return xms_err(XMSE_NOHMA);
    case 0x03:                                  /* global enable A20 */
        x86_a20_set(1);
        return xms_ok();
    case 0x04:                                  /* global disable A20 */
        if (xms_a20_local)
            return xms_err(XMSE_NOTALLOC);
        x86_a20_set(0);
        return xms_ok();
    case 0x05:                                  /* local enable A20 */
        /* Nesting, because a driver that enables locally twice and disables
           once expects the gate to still be open. One state, the machine's --
           x86_a20_set() and x86_a20_changes, the same gate the 8042 path in
           x86_io.c drives. There is no second A20 model here. */
        xms_a20_local++;
        x86_a20_set(1);
        return xms_ok();
    case 0x06:                                  /* local disable A20 */
        if (xms_a20_local)
            xms_a20_local--;
        if (!xms_a20_local)
            x86_a20_set(0);
        return xms_ok();
    case 0x07:                                  /* query A20 state */
        x86_set_ax((uint16_t)x86_a20_state);
        xms_bl_set(0);
        return 1;

    case 0x08: {                                /* query free extended memory */
        uint32_t largest = xms_largest_free(), total = xms_free_kb();
        x86_set_ax((uint16_t)(largest > 0xFFFFu ? 0xFFFFu : largest));
        x86_set_dx((uint16_t)(total > 0xFFFFu ? 0xFFFFu : total));
        if (largest == 0) {
            xms_bl_set(XMSE_NOMEM);
            x86_xms_last_err = XMSE_NOMEM;
        } else {
            xms_bl_set(0);
        }
        return 1;
    }

    case 0x09: {                                /* allocate */
        unsigned i;
        for (i = 0; i < X86_XMS_HANDLES; i++)
            if (!xms_h[i].used)
                break;
        if (i == X86_XMS_HANDLES)
            return xms_err(XMSE_NOHANDLE);
        uint32_t off = xms_find_gap(dx);
        if (off == XMS_NO_OFF)
            return xms_err(XMSE_NOMEM);
        xms_h[i].used = 1;
        xms_h[i].locks = 0;
        xms_h[i].kb_off = off;
        xms_h[i].kb = dx;
        xms_account();
        xms_ok();
        x86_set_dx((uint16_t)(i + 1));
        return 1;
    }

    case 0x0A: {                                /* free */
        xms_handle_t *h = xms_get(dx);
        if (!h)
            return xms_err(XMSE_BADHANDLE);
        if (h->locks)
            return xms_err(XMSE_LOCKED);
        h->used = 0;
        h->kb = 0;
        xms_account();
        return xms_ok();
    }

    case 0x0B:
        return xms_move();

    case 0x0C:                                  /* lock a block */
        if (!xms_get(dx))
            return xms_err(XMSE_BADHANDLE);
        /* Honest refusal: a lock exists to yield a 32-bit LINEAR address, and
           our bytes are in a file. See x86_xms.h. */
        return xms_err(XMSE_LOCKFAIL);
    case 0x0D:                                  /* unlock */
        if (!xms_get(dx))
            return xms_err(XMSE_BADHANDLE);
        return xms_err(XMSE_NOTLOCKED);

    case 0x0E: {                                /* get handle information */
        xms_handle_t *h = xms_get(dx);
        if (!h)
            return xms_err(XMSE_BADHANDLE);
        unsigned free_h = 0;
        for (unsigned i = 0; i < X86_XMS_HANDLES; i++)
            if (!xms_h[i].used)
                free_h++;
        uint32_t kb = h->kb;
        x86_set_ax(1);
        xms_bh_set(h->locks);
        xms_bl_set((uint8_t)free_h);
        x86_set_dx((uint16_t)kb);
        return 1;
    }

    case 0x0F: {                                /* reallocate */
        xms_handle_t *h = xms_get(dx);
        if (!h)
            return xms_err(XMSE_BADHANDLE);
        if (h->locks)
            return xms_err(XMSE_LOCKED);
        if (bx <= h->kb) {                      /* shrink in place */
            h->kb = bx;
            xms_account();
            return xms_ok();
        }
        /* Grow. Search with this block taken out of the picture, so growing in
           place is found first; only a move costs a store-to-store copy. */
        uint32_t want = bx;
        uint32_t old_off = h->kb_off, old_kb = h->kb;
        h->used = 0;
        uint32_t off = xms_find_gap(want);
        h->used = 1;
        if (off == XMS_NO_OFF)
            return xms_err(XMSE_NOMEM);
        if (off != old_off && old_kb) {
            if (xms_store_begin(1) != 0)
                return xms_err(XMSE_PARITY);
            int rc = xms_store_run(off << 10, old_off << 10, old_kb << 10);
            xms_store_end();
            if (rc != 0)
                return xms_err(XMSE_PARITY);
        }
        h->kb_off = off;
        h->kb = want;
        xms_account();
        return xms_ok();
    }

    default:
        /* Loud, like every other unimplemented service in this tree. */
        x86_xms_unhandled = func;
        return xms_err(XMSE_NOFUNC);
    }
}

/* ---- INT 2Fh -------------------------------------------------------------
 * ONLY AX=4300h and AX=4310h. Every other multiplex subfunction returns with
 * the guest's registers exactly as they arrived, which is what the bare IRET
 * this replaces did and what "no handler" is defined to look like.
 */
int x86_xms_int2f(void)
{
    uint16_t ax = (uint16_t)cpu_state_ptr->regs[0];

    if (ax == 0x4300) {
        /* AL=80h is the whole detection protocol. With no store we are NOT
           present -- advertising a driver with nothing behind it is worse than
           being absent, because the guest then stops looking elsewhere. */
        if (!xms_kb)
            return 1;
        x86_set_al(0x80);
        x86_xms_detects++;
        return 1;
    }
    if (ax == 0x4310) {
        if (!xms_kb)
            return 1;
        cpu_state_ptr->segs[0].sel  = X86_XMS_ENTRY_SEG;   /* ES */
        cpu_state_ptr->segs[0].base = SEG(X86_XMS_ENTRY_SEG);
        x86_set_bx(X86_XMS_ENTRY_OFF);
        x86_xms_entry_reqs++;
        return 1;
    }
    return 1;
}
