/*
 * Target half of the probe accumulator. Design and cost: x86_probe.h.
 *
 * The whole file is inside #if X86_PROBE so a normal build emits neither code
 * nor storage -- not even the 2.8 KB block, which matters because RAM_EMU
 * headroom in this tree is small enough to have gone negative unnoticed.
 */
#include "x86_probe.h"

#if X86_PROBE

#include <string.h>

/*
 * Deliberately UNINITIALISED, so it lands in .bss rather than .data. A static
 * initialiser here would put all 2,848 bytes of it into the packed dos.bin as
 * well as into RAM -- paying twice for a diagnostic. The header is stamped on
 * first use instead; the host distinguishes "not built with X86_PROBE" from
 * "built but never called" by whether the SYMBOL exists in the ELF, not by the
 * magic.
 */
x86_probe_t x86_probe;

/*
 * Which second byte discriminates a service, if any.
 *
 * Keying every INT on AL would be wrong in the expensive direction: INT 21h
 * AH=02h (character output) would take one slot per character printed and fill
 * the table with the screen's contents. Keying none of them would be wrong in
 * the direction that actually hurts: INT 10h AH=00h AL=0Dh (set mode 0Dh) and
 * AL=03h (set mode 3) are different questions with different answers, and a
 * report that merges them cannot say which mode was refused.
 *
 * So it is a curated list, and the default is "do not key". 0xFF means none.
 * INT 10h AH=12h is the odd one: its subfunction is in BL, not AL, which is
 * why this returns a BYTE rather than a boolean.
 */
static uint8_t probe_sub(uint8_t vector, uint8_t ah, uint8_t al, uint8_t bl)
{
    if (vector == 0x10) {
        switch (ah) {
        case 0x00:      /* set mode: AL IS the question */
        case 0x10:      /* palette/DAC: AL selects among a dozen calls */
        case 0x11:      /* character generator */
        case 0x1A:      /* display combination */
            return al;
        /* NOT AH=0Fh. AL is an OUTPUT there (it returns the mode), so keying
           on it splits one service across a row per incoming junk value --
           which is what a first run of this instrument actually did. */
        case 0x12:      /* EGA/VGA detection cascade: BL selects */
            return bl;
        default:
            return 0xFF;
        }
    }
    if (vector == 0x21) {
        switch (ah) {
        case 0x44:      /* IOCTL: AL is a whole family of calls */
        case 0x43:      /* get (AL=0) vs set (AL=1) attributes */
        case 0x4B:      /* EXEC: load-and-go vs load-only */
        case 0x42:      /* seek origin */
        case 0x48:      /* (no subfunction, but AL is often junk) -- skip */
            return (ah == 0x48) ? 0xFF : al;
        default:
            return 0xFF;
        }
    }
    if (vector == 0x15)
        return al;      /* the misc BIOS: AL discriminates throughout */
    return 0xFF;
}

/*
 * Open-addressed index over the entries. Linear scan would be correct but port
 * I/O is not rare -- one title made 15,228 port accesses in six seconds -- and
 * an O(used) scan per access turns a diagnostic into a slowdown that changes
 * what the guest does.
 */
static unsigned probe_hash(uint8_t kind, uint16_t id, uint8_t ah, uint8_t sub)
{
    uint32_t h = ((uint32_t)kind << 26) ^ ((uint32_t)id << 10)
               ^ ((uint32_t)ah << 3) ^ sub;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    return (h >> 19) & (X86_PROBE_HASH - 1u);
}

static void probe_maybe_reset(void)
{
    if (x86_probe.magic != X86_PROBE_MAGIC) {
        x86_probe.magic = X86_PROBE_MAGIC;
        x86_probe.version = X86_PROBE_VERSION;
        x86_probe.ents = X86_PROBE_ENTS;
        x86_probe.reset = 1;            /* fall through and zero the rest */
    }
    if (!x86_probe.reset)
        return;
    memset(x86_probe.hash, 0, sizeof(x86_probe.hash));
    memset(x86_probe.ent, 0, sizeof(x86_probe.ent));
    x86_probe.used = 0;
    x86_probe.dropped = 0;
    x86_probe.calls = 0;
    x86_probe.reset = 0;
}

static x86_probe_ent_t *probe_slot(uint8_t kind, uint16_t id, uint8_t ah,
                                   uint8_t sub)
{
    unsigned h = probe_hash(kind, id, ah, sub);
    for (unsigned i = 0; i < X86_PROBE_HASH; i++) {
        unsigned s = (h + i) & (X86_PROBE_HASH - 1u);
        unsigned slot = x86_probe.hash[s];
        if (slot == 0) {
            /* Free bucket: claim an entry, unless we are full. FULL SATURATES:
               the table stops taking new keys and counts the refusals, so the
               report says "N keys were dropped" rather than presenting a
               silently truncated list as the whole conversation. */
            if (x86_probe.used >= X86_PROBE_ENTS || x86_probe.used >= 255) {
                x86_probe.dropped++;
                return 0;
            }
            unsigned idx = x86_probe.used++;
            x86_probe.hash[s] = (uint8_t)(idx + 1);
            x86_probe_ent_t *e = &x86_probe.ent[idx];
            e->id = id;
            e->kind = kind;
            e->ah = ah;
            e->sub = sub;
            return e;
        }
        x86_probe_ent_t *e = &x86_probe.ent[slot - 1];
        if (e->kind == kind && e->id == id && e->ah == ah && e->sub == sub)
            return e;
    }
    x86_probe.dropped++;
    return 0;
}

static void probe_bump(x86_probe_ent_t *e, unsigned flags)
{
    if (e->count != 0xFFFFFFFFu)
        e->count++;
    e->flags |= (uint8_t)flags;
}

/*
 * Vectors that take NO function number in AH: a hardware interrupt, or a call
 * whose whole meaning is the vector. AH there is whatever the guest happened to
 * be holding, and keying on it turns one service into a row per passing
 * register value -- the first run of this instrument produced seven rows for
 * INT 08h ("AH=DAh", "AH=C1h", ...) which are the timer tick, seven times.
 */
static int probe_ah_is_junk(uint8_t vector)
{
    return vector == 0x08 || vector == 0x09 || vector == 0x20;
}

void x86_probe_int(uint8_t vector, uint8_t ah, uint8_t sub, int rc,
                   unsigned marked, uint16_t ax, uint16_t bx, uint32_t cx_dx,
                   int cf)
{
    if (probe_ah_is_junk(vector)) {
        ah = 0;
        sub = 0xFF;
    }
    probe_maybe_reset();
    x86_probe.calls++;
    x86_probe_ent_t *e = probe_slot(X86_PROBE_KIND_INT, vector, ah, sub);
    if (!e)
        return;
    unsigned f = (rc == 0) ? X86_PROBE_F_UNANSWERED
               : (rc == 2) ? X86_PROBE_F_RETRY : X86_PROBE_F_ANSWERED;
    if (marked)
        f |= X86_PROBE_F_MARKED;
    f |= cf ? X86_PROBE_F_CF_SET : X86_PROBE_F_CF_CLEAR;
    probe_bump(e, f);
    e->ax = ax;
    e->bx = bx;
    e->cx_dx = cx_dx;
}

uint8_t x86_probe_sub(uint8_t vector, uint8_t ah, uint8_t al, uint8_t bl)
{
    return probe_sub(vector, ah, al, bl);
}

void x86_probe_port(unsigned kind, uint16_t port, uint32_t value,
                    unsigned unmodelled)
{
    probe_maybe_reset();
    x86_probe.calls++;
    x86_probe_ent_t *e = probe_slot((uint8_t)kind, port, 0, 0xFF);
    if (!e)
        return;
    probe_bump(e, unmodelled ? X86_PROBE_F_NOPORT : X86_PROBE_F_ANSWERED);
    e->ax = (uint16_t)value;
    /* cx_dx doubles as "the set of distinct low bytes seen", which is what
       separates a register file being PROGRAMMED from one being polled: 32,000
       reads of 0x3DA that only ever returned 0x01 and 0x09 is a retrace poll,
       and the same count with forty distinct values is not. Cheap: a 32-bit
       OR-fold of (1 << (value & 31)). */
    e->cx_dx |= 1u << (value & 31u);
    if (kind == X86_PROBE_KIND_OUT)
        e->bx = (uint16_t)value;    /* last value WRITTEN, kept distinctly */
}

#endif /* X86_PROBE */
