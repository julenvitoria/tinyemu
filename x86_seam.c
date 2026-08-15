/*
 * The granule seam.
 *
 * ---- What the defect is ---------------------------------------------------
 *
 * The fold answers per 4 KB granule: host = lin + k, one k per page. Two
 * adjacent GUEST granules are adjacent in HOST memory only by luck. The pager
 * hands frames out in fault order; copy-on-write moves a granule into a pool
 * page; a fixed mapping's neighbour is a different device entirely. So a guest
 * 16- or 32-bit access whose bytes span a granule boundary resolves ONE pointer
 * -- from the LOW granule's k -- and then reads or writes straight past the top
 * of that host page, into memory belonging to an unrelated guest page.
 *
 * It is silent. Nothing faults, nothing is counted, the guest simply gets bytes
 * that are not its own. The prior DOS port hit exactly this and named it "the
 * seam": a straddling 16-bit DATA read is what made KEEN4 and TOPBENCH diverge
 * there (references/dos_port_prior/CLAUDE.md, "The memory map";
 * references/8086tiny/8086tiny.c:7176-7183). Here it is what breaks
 * Wolfenstein 3-D at any PSP that is not 4 KB aligned: shift the load address
 * by a whole granule and every structure keeps its offset within its page, so
 * the same accesses straddle and the same luck holds; shift it by 256 bytes and
 * a different set of accesses lands on the boundary.
 *
 * ---- The mechanism: pull, then push --------------------------------------
 *
 * A bounce buffer cannot serve this. A folded operand pointer is not consumed
 * on the spot -- handlers do arithmetic on it (the second word of a far
 * pointer, the string helpers) -- and the prior port TRIED a gather buffer for
 * data and it turned their suite red (8086tiny.c:11360-11366, "Do not re-try
 * it"). So the pointer handed back is always the REAL one, and what changes is
 * the memory it points into:
 *
 *   PULL, before the access. Every frame carries X86_PG_SLACK bytes past its
 *         top that belong to nobody (x86_pager.h). Copy the neighbour granule's
 *         leading bytes into them. A straddling READ is now correct.
 *
 *   PUSH, at the next instruction boundary. Compare the slack against the
 *         snapshot taken at pull time and store back only the bytes that
 *         CHANGED. A straddling WRITE now lands. Byte-wise and conditional, so
 *         a pure read never dirties the neighbour.
 *
 * The instruction boundary is the correct flush point because no operand
 * pointer the decoder produces outlives the instruction that decoded it
 * (8086tiny.c:21255-21259). Four slots cover the worst instruction: a source
 * operand, a destination operand, and a string op's source and destination.
 *
 * ---- What it does NOT do, and why that is LOUD ---------------------------
 *
 * Only a PAGER FRAME has slack. A granule that is a fixed mapping (VRAM, the
 * mode 13h aperture, the open-bus filler, the BIOS id page) or is still being
 * read out of NOR has no eight bytes of its own past its top, and its neighbour
 * is not ours to write. x86_seam_sync() refuses those and counts the refusal in
 * x86_seam_overflow. That is the prior port's degradation exactly -- it counted
 * `dos_seam_overflow` for a full pool and a full slot table rather than
 * guessing -- and the counter being non-zero is a fact to read, not a silence.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_pager.h"
#include "x86_arena.h"
#include "x86_vprof.h"

#define X86_SEAM_SLOTS 4

typedef struct {
    uint32_t  page;                       /* the LOW granule of the boundary */
    uint8_t  *slack;                      /* its eight bytes past the top    */
    uint8_t   orig[X86_PG_SLACK];         /* what the pull put there         */
} seam_slot_t;

static seam_slot_t seam_slot[X86_SEAM_SLOTS];

/* Host-readable, and the whole instrument. `syncs` is how often a straddle was
   SERVED, `writes` how many bytes a straddling store actually pushed back, and
   `overflow` how often the seam could not be served at all -- a full slot
   table, or a boundary with no slack. A non-zero overflow is not a crash; it is
   the one number that says a straddling access fell back to the old, wrong
   behaviour, and it must never be inferred from silence. */
volatile uint32_t x86_seam_syncs    = 0;
volatile uint32_t x86_seam_writes   = 0;
volatile uint32_t x86_seam_overflow = 0;
volatile uint32_t x86_seam_flushes  = 0;

/*
 * STICKY POOL EXHAUSTION, and it is here because the prior port measured what
 * happens without it: a single unservable boundary re-asked an empty pool once
 * per straddling access and turned their `dos_cow_lost` into 16,392
 * (references/8086tiny/8086tiny.c:11798-11812). Ours would inflate
 * x86_pg_exhausted the same way and make the PAGER's instrument unreadable --
 * the counter would then be describing the seam's retries rather than the
 * guest's memory pressure. Cleared the moment a frame is free again, so this
 * degrades and recovers instead of latching for the rest of the run.
 */
static uint8_t seam_pool_full;

/* Live slot count. Kept in the CPU context rather than here so the interpreter
   can test it at the loop head with one DTCM load off R8 instead of
   materialising a literal address on every guest instruction. */

/* The vprof bracket is a WRAPPER, not an edit of the body, and it disappears
   completely when X86_VPROF is 0 -- x86_seam_sync then compiles to exactly the
   code it compiled to before this instrument existed. A split into a wrapper
   plus a body that GCC re-inlines does NOT give that: it reorders registers and
   the shipping dos.bin stops being byte-identical, which is the one thing a
   profiling flag is required not to do. */
#if X86_VPROF
static void seam_sync_body(uint32_t lin);

void x86_seam_sync(uint32_t lin)
{
    X86_VPROF_BEGIN(sm);
    seam_sync_body(lin);
    X86_VPROF_END(sm, seam);
}

static void seam_sync_body(uint32_t lin)
#else
void x86_seam_sync(uint32_t lin)
#endif
{
    uint32_t page = lin >> FOLD_SHIFT;

    if (page + 1u >= FOLD_ENTRIES)
        return;                       /* nothing above it to straddle into */

    uint32_t k  = guest_page_table_ptr[page];
    uint32_t k1 = guest_page_table_ptr[page + 1u];

    /*
     * THE CONTIGUITY EARLY-OUT, and the reason this costs nothing on a machine
     * whose memory is laid out flat. Equal k means the two granules are one
     * contiguous host run: the bytes past the top ARE the neighbour and there
     * is nothing to do. It is the common case in the tier-1 harness and in any
     * build whose guest RAM is mapped 1:1.
     *
     * BOTH BEING FOLD_INVALID COMPARES EQUAL AND IS NOT CONTIGUITY -- that is
     * a sentinel, not an offset. The prior port shipped that hole and found it
     * by inspection (8086tiny.c:11769-11784). Excluded here explicitly.
     */
    if (k == k1 && k != FOLD_INVALID)
        return;

    /* The low granule must be resident with slack of its own before anything
       can be pulled into it. A fold miss here is the ordinary demand fault --
       serve it, as a WRITE, because the whole point of the slack is that the
       instruction may store through it. */
    if (k == FOLD_INVALID) {
        if (seam_pool_full && !x86_pg_frames_free) {
            x86_seam_overflow++;         /* already known empty: do not re-ask */
            return;
        }
        if (!x86_pager_fault(lin, 1)) {
            seam_pool_full = 1;
            x86_seam_overflow++;
            return;
        }
        seam_pool_full = 0;
    }

    uint8_t *slack = x86_pager_slack(page);
    if (!slack) {
        /* No frame, so no eight bytes that belong to nobody: a fixed mapping or
           a NOR-backed page. Refused and COUNTED -- see the block at the top. */
        x86_seam_overflow++;
        return;
    }

    /* The neighbour has to be readable before its leading bytes can be pulled.
       Faulting it in here is not recursion: x86_pager_fault() does not fold, it
       fills a frame and installs an entry. */
    if (k1 == FOLD_INVALID) {
        if (seam_pool_full && !x86_pg_frames_free) {
            x86_seam_overflow++;
            return;
        }
        if (!x86_pager_fault((page + 1u) << FOLD_SHIFT, 1)) {
            seam_pool_full = 1;
            x86_seam_overflow++;
            return;
        }
        seam_pool_full = 0;
        k1 = guest_page_table_ptr[page + 1u];
        if (k1 == FOLD_INVALID) {
            x86_seam_overflow++;
            return;
        }
    }

    /* Already synced this boundary during this instruction: the slack is LIVE
       and a second pull would overwrite a store the instruction just made.
       The dedupe is ahead of the slot-full test deliberately -- the prior port
       had it the other way round and charged an already-synced boundary as an
       overflow, i.e. reported a correct access as a degraded one
       (8086tiny.c:11820-11831). */
    uint32_t n = cpu_state_ptr->seam_n;
    for (uint32_t s = 0; s < n; s++)
        if (seam_slot[s].page == page)
            return;

    if (n >= X86_SEAM_SLOTS) {
        x86_seam_overflow++;
        return;
    }

    /* Both granules now hold a raw host pointer that stays live until the
       flush. Tell the pager's victim picker so neither can be evicted under
       it -- the same live-pointer hazard x86_arena.h's livemap exists for. */
    x86_livemap_note(page);
    x86_livemap_note(page + 1u);

    const uint8_t *nb =
        (const uint8_t *)x86_fold_ptr((page + 1u) << FOLD_SHIFT, k1);

    memcpy(slack, nb, X86_PG_SLACK);
    memcpy(seam_slot[n].orig, slack, X86_PG_SLACK);
    seam_slot[n].page  = page;
    seam_slot[n].slack = slack;
    cpu_state_ptr->seam_n = n + 1u;
    x86_seam_syncs++;
}

void x86_seam_flush(void)
{
    uint32_t n = cpu_state_ptr->seam_n;

    x86_seam_flushes++;
    for (uint32_t s = 0; s < n; s++) {
        uint32_t nb = (seam_slot[s].page + 1u) << FOLD_SHIFT;
        uint32_t k1 = guest_page_table_ptr[seam_slot[s].page + 1u];

        if (k1 == FOLD_INVALID) {
            /* The neighbour went away between the pull and the push. Nothing
               can be pushed; say so rather than write through a sentinel. */
            x86_seam_overflow++;
            continue;
        }
        uint8_t *dst = (uint8_t *)x86_fold_ptr(nb, k1);
        for (uint32_t i = 0; i < X86_PG_SLACK; i++)
            if (seam_slot[s].slack[i] != seam_slot[s].orig[i]) {
                dst[i] = seam_slot[s].slack[i];
                x86_seam_writes++;
            }
    }
    cpu_state_ptr->seam_n = 0;
}

/*
 * THE CODE-STREAM SEAM, and why it is a DIFFERENT mechanism to the data one
 * above even though it is the same defect.
 *
 * The dispatch folds the opcode byte and threads that granule's k to every
 * later fetch of the same instruction (cpu->code_k, see cpu_exec.S). That is
 * only sound while the whole instruction lives in the opcode's granule, so the
 * dispatch proves `offset <= 0xFF0` -- 16 bytes, the longest x86 instruction --
 * and lands HERE when it cannot.
 *
 * The pull/push slack cannot serve this case. X86_PG_SLACK is EIGHT bytes and
 * an opcode at offset 0xFFF can be followed by fourteen more, so the slack is
 * short by design; and slack is refused outright for a fixed mapping or a
 * NOR-backed page, which is exactly where a boot stub or an XIP'd overlay
 * executes. A refusal there would be a wrong INSTRUCTION, not a wrong operand.
 *
 * So the instruction is STAGED instead: sixteen bytes, each folded on its own,
 * copied into DTCM, and code_k re-pointed at the buffer. Every fetch site is
 * then byte-identical to the fast path -- `LDR code_k` + `ADD R10` -- and there
 * is no per-site slow path anywhere in the interpreter. The buffer is only
 * read, never written, so nothing has to be pushed back at the boundary; the
 * next dispatch re-folds and overwrites it.
 *
 * A byte that cannot be folded at all stages as ZERO rather than faulting. The
 * fetch that actually needs it is what must fault, and it cannot: the guest may
 * legitimately have a two-byte instruction two bytes below an unmapped page.
 * Staging zero and letting the real length decide is the only correct order.
 * x86_code_reloc_faults counts those bytes so the case is not silent.
 */
volatile uint32_t x86_code_relocs      = 0;
volatile uint32_t x86_code_reloc_faults = 0;

void *x86_code_reloc(uint32_t lin)
{
    uint8_t *buf = cpu_state_ptr->code_buf;
    X86_VPROF_BEGIN(rl);

    x86_code_relocs++;
    for (uint32_t i = 0; i < sizeof(cpu_state_ptr->code_buf); i++) {
        uint32_t a = lin + i;
        uint32_t k = guest_page_table_ptr[a >> FOLD_SHIFT];

        if (k == FOLD_INVALID) {
            x86_pager_fault(a, 0);
            k = guest_page_table_ptr[a >> FOLD_SHIFT];
            if (k == FOLD_INVALID) {
                x86_code_reloc_faults++;
                buf[i] = 0;
                continue;
            }
        }
        /* A NOR-backed granule folds to 0x9xxxxxxx, which is memory-mapped XIP
           and plainly readable -- the write trap exists for stores, and there
           are none here.

           x86_fold_ptr(), NOT a raw `a + k`. While a planar mode is armed the
           sixteen aperture entries hold X86_FOLD_VGA_K, which is
           0x80000000 + host and so is NOT equal to FOLD_INVALID -- the guard
           above passes it through, and `a + k` then yields 0xAxxxxxxx, which
           is Cortex-M7 device space and hard-faults on silicon. Reachable two
           ways: CS:EIP in 0x9FFF1..0x9FFFF, where the sixteen-byte staging
           window spills into 0xA0000; and CS:EIP inside the aperture, which
           MEM_TRAP_R_CODE (cpu_exec.S) already handles for the opcode byte
           before handing the REST of the instruction to this function. The
           two sister walks below/above use x86_fold_ptr for this reason and
           this one was missed. CLAUDE.md's rule, and the pager's slack prime
           is the incident that produced it. */
        buf[i] = *(const volatile uint8_t *)x86_fold_ptr(a, k);
    }

    cpu_state_ptr->code_k = (uint32_t)(uintptr_t)buf - lin;
    X86_VPROF_END(rl, reloc);
    return buf;
}

void x86_seam_reset(void)
{
    seam_pool_full = 0;
    if (cpu_state_ptr)
        cpu_state_ptr->seam_n = 0;
}
