/*
 * x86_probe.h -- what the guest ASKED for, and how we answered.
 *
 * X86_PROBE=1, off by default like every other instrument in this tree.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS NOT ANOTHER RING
 * ---------------------------------------------------------------------------
 * There are already nine trace rings here (x86_dos_file_log, x86_io_log,
 * x86_hle_log, x86_int_vec_log, ...) and each was built for one investigation.
 * Every one of them shares the same two defects when the question is "what did
 * this title probe, and what did we hand back":
 *
 *   * They WRAP. A 32-slot ring spans milliseconds of a guest that polls, so
 *     the call that mattered -- the one probe issued once, at startup -- is
 *     always gone by the time anyone looks. That cost one investigation hours:
 *     entry 2 had been overwritten by 81 identical later ones.
 *   * They record the REQUEST and not the ANSWER'S PROVENANCE. "INT 21h AH=43h
 *     returned CF=1 AX=0001" reads identically whether a real handler decided
 *     that or a default arm invented it. That distinction is the whole point of
 *     this file.
 *
 * So this is an ACCUMULATOR keyed by (kind, id, AH, sub), not a ring:
 *
 *   * It cannot lose the early entries, because it never evicts. A title that
 *     makes 400 calls to twelve distinct services occupies twelve slots.
 *   * When it does fill up it SATURATES and counts what it dropped, so the
 *     report can say so instead of quietly showing a subset.
 *   * Every slot carries the flags that say how the answer was produced.
 *
 * The existing rings are untouched and still useful: they carry ORDER, which an
 * accumulator throws away. Use --dos-log / --io-log when the question is "in
 * what sequence"; use this when the question is "what, and did we answer it".
 *
 * ---------------------------------------------------------------------------
 * WHAT "HOW WE ANSWERED" MEANS, AND WHERE EACH BIT COMES FROM
 * ---------------------------------------------------------------------------
 * Two independent sources, deliberately:
 *
 *   RUNTIME EVIDENCE (this file). A service that returned rc==0, or that moved
 *   x86_hle_unhandled_vector/_func, did not answer. A port read that fell off
 *   the end of port_read_inner's switch was never modelled. These are FACTS
 *   the target observed and they cannot drift.
 *
 *   CURATED KNOWLEDGE (scripts/common/probe_expect.py). Whether a handler that
 *   DID return rc==1 answered honestly, stubbed, or fell into a shaped-nonsense
 *   default arm is a property of the source, not of the run. Encoding it on the
 *   target would mean an annotation macro at every one of those arms in
 *   x86_bios.c and x86_dos.c -- files under active change. It lives host-side
 *   instead, and anything NOT in that table is reported as `unclassified`,
 *   which is itself actionable: nobody has vetted that answer.
 *
 * ---------------------------------------------------------------------------
 * COST
 * ---------------------------------------------------------------------------
 * Zero when X86_PROBE=0: every macro compiles to nothing and no storage is
 * emitted. When on, the storage is X86_PROBE_ENTS * 16 bytes plus a 256-byte
 * hash index plus a 32-byte header -- 2,848 B at the default 160 entries, in
 * RAM_EMU. The exact link delta is measured and printed by
 * scripts/probe_report.py, because RAM_EMU headroom has gone negative silently
 * in this tree before and a claimed cost is not a measured one.
 *
 * AND IT DID GO NEGATIVE, THROUGH THIS FILE. MEASURED: with the accumulator in,
 * RAM_EMU had 655,360 B free at the guest_ram claim and the 160-frame pager
 * pool wants 656,640 (stride 4,104), so the claim -- the LAST one out of that
 * pool -- returned NULL. x86_pg_frames_total was 0, every guest access was
 * X86_EXIT_MEM_FAULT, the guest halted before its first instruction, and
 * core_dtcm_ok/pgf_result/x86_xms_total_kb all still read healthy. The report
 * said "every service this title touched was answered" about a guest that had
 * never run, for Dark Forces and then for WOLF3D as a control.
 *
 * Two things changed and BOTH matter:
 *   * src/main.c sizes the pool to the free space instead of taking it
 *     all-or-nothing, and publishes guest_ram_pages / guest_ram_pages_short.
 *     The instrumented build now runs on 159 frames (x86_pg_evictions 0 on
 *     WOLF3D) instead of on none.
 *   * probe_report.py REFUSES to print a table when core_ready is 0,
 *     x86_pg_frames_total is 0, or guest_insns_total is not advancing.
 * X86_PROBE_ENTS=64 saves exactly the 1,280 B and buys the frame back if the
 * pager is what is under investigation; see Makefile.core.
 *
 * THE GENERAL RULE FOR THE NEXT INSTRUMENT: RAM_EMU .bss is taken from the
 * guest's page pool, one frame per 4,104 bytes. Do not add storage here
 * without reading guest_ram_pages_short afterwards.
 *
 * The per-call cost is one hash probe on paths that already cost hundreds of
 * cycles (an INT service, a port access). It is NOT on the guest instruction
 * dispatch path, which is what keeps it two orders of magnitude cheaper than
 * X86_PROFILE.
 */
#ifndef X86_PROBE_H
#define X86_PROBE_H

#include <stdint.h>

#ifndef X86_PROBE
#define X86_PROBE 0
#endif

/* Kinds. Keep in step with KINDS in scripts/common/probe_report.py. */
#define X86_PROBE_KIND_INT 0
#define X86_PROBE_KIND_IN  1
#define X86_PROBE_KIND_OUT 2

/* Flags: how the answer was produced, as the TARGET saw it. Accumulated with
   OR across every call to the same key, so a service that answered 99 times
   and defaulted once still shows the default -- a "last one wins" field would
   hide exactly the call worth finding. */
#define X86_PROBE_F_ANSWERED   0x01u   /* rc == 1 at least once */
#define X86_PROBE_F_UNANSWERED 0x02u   /* rc == 0: nobody claimed it */
#define X86_PROBE_F_MARKED     0x04u   /* moved x86_hle_unhandled_* */
#define X86_PROBE_F_RETRY      0x08u   /* rc == 2: blocking, re-executed */
#define X86_PROBE_F_CF_SET     0x10u   /* returned CF=1 (an error) at least once */
#define X86_PROBE_F_CF_CLEAR   0x20u   /* returned CF=0 (success) at least once */
#define X86_PROBE_F_NOPORT     0x40u   /* port I/O: fell off the switch */

/* 128 is not a guess: the busiest title measured here claimed 31 slots, and a
   table that is comfortably larger than the worst observed case is the point of
   an accumulator. It saturates rather than evicting if that is ever wrong, and
   the report says how many keys it refused. */
#ifndef X86_PROBE_ENTS
#define X86_PROBE_ENTS 128
#endif
#define X86_PROBE_HASH 256            /* power of two, <= 255 usable indices */

#define X86_PROBE_MAGIC   0x50524F42u /* 'PROB' */
#define X86_PROBE_VERSION 1

/* 20 bytes. Field order is the on-wire layout the host decoder mirrors; do not
   reorder without changing scripts/common/probe_acc.py in the same commit.
   `count` is 32-bit deliberately: a 16-bit one CEILINGED at 65,535 on the first
   real title measured (Alley Cat polls port 0x40 far past that), and a count
   that silently stops is a count that misleads. The four extra bytes are paid
   for by dropping the table from 160 entries to 128. */
typedef struct {
    uint16_t id;        /* INT vector, or port number */
    uint8_t  kind;
    uint8_t  ah;        /* AH for an INT; 0 for a port */
    uint8_t  sub;       /* the discriminating second byte, or 0xFF for "none" */
    uint8_t  flags;
    uint16_t ax;        /* last AX returned, or the port value */
    uint32_t count;
    uint16_t bx;        /* last BX returned */
    uint16_t pad;
    uint32_t cx_dx;     /* last (CX << 16) | DX */
} x86_probe_ent_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t reset;     /* host writes 1; the target zeroes and clears it */
    uint32_t ents;      /* X86_PROBE_ENTS, so the host never assumes */
    uint32_t used;
    uint32_t dropped;   /* keys refused because the table was full */
    uint32_t calls;     /* every recorded event, whether it got a slot or not */
    uint32_t reserved;
    uint8_t  hash[X86_PROBE_HASH];
    x86_probe_ent_t ent[X86_PROBE_ENTS];
} x86_probe_t;

#if X86_PROBE

extern x86_probe_t x86_probe;

void x86_probe_int(uint8_t vector, uint8_t ah, uint8_t sub, int rc,
                   unsigned marked, uint16_t ax, uint16_t bx, uint32_t cx_dx,
                   int cf);
void x86_probe_port(unsigned kind, uint16_t port, uint32_t value,
                    unsigned unmodelled);
/* Which second byte discriminates this service, or 0xFF for none. The policy
   lives in x86_probe.c next to the table it feeds; see the comment there. */
uint8_t x86_probe_sub(uint8_t vector, uint8_t ah, uint8_t al, uint8_t bl);

#define X86_PROBE_INT(v, ah, sub, rc, mk, ax, bx, cxdx, cf) \
    x86_probe_int((v), (ah), (sub), (rc), (mk), (ax), (bx), (cxdx), (cf))
#define X86_PROBE_PORT(kind, port, val, unmod) \
    x86_probe_port((kind), (port), (val), (unmod))

#else

#define X86_PROBE_INT(v, ah, sub, rc, mk, ax, bx, cxdx, cf) ((void)0)
#define X86_PROBE_PORT(kind, port, val, unmod)              ((void)0)

#endif /* X86_PROBE */

#endif /* X86_PROBE_H */
