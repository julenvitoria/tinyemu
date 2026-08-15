/*
 * LIM EMS 3.2 -- the mechanism. The design, the residency rule and the reason
 * this is the opposite shape to XMS are all in x86_ems.h.
 *
 * This is a translation unit of its own, unlike XMS which lives inside
 * x86_dos.c: EMS is not a DOS service. It is a memory MAPPING that happens to
 * be reached through an interrupt, it touches the fold table and the pager's
 * pin bitmap directly, and putting that next to the FatFs file handlers would
 * hide what it is.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_arena.h"
#include "x86_pager.h"
#include "x86_ems.h"

volatile uint32_t x86_ems_present_v;
volatile uint32_t x86_ems_frame_refused;
volatile uint32_t x86_ems_total_pages;
volatile uint32_t x86_ems_used_pages;
volatile uint32_t x86_ems_handles_used;
volatile uint32_t x86_ems_device_opens;
volatile uint32_t x86_ems_calls;
volatile uint32_t x86_ems_maps;
volatile uint32_t x86_ems_unmaps;
volatile uint32_t x86_ems_map_fails;
volatile uint32_t x86_ems_pinned_pages;
volatile uint32_t x86_ems_last_err;
volatile uint32_t x86_ems_unhandled;

#define EMS_NO_PAGE  0xFFFFu

typedef struct {
    uint8_t  used;
    uint8_t  saved;                 /* AH=47h has stored a map for it       */
    uint16_t first;                 /* first GLOBAL logical page it owns    */
    uint16_t count;
    uint16_t save_h[X86_EMS_WINDOWS];
    uint16_t save_l[X86_EMS_WINDOWS];
} ems_handle_t;

static ems_handle_t ems_h[X86_EMS_HANDLES];

/* What each window currently shows. EMS_NO_PAGE in `lpage` means unmapped, and
   then `gpage` is meaningless. `gpage` is the GLOBAL logical page, which is what
   the backing address is derived from -- keeping the handle-relative number as
   well is what lets AH=4Eh report the map the way the guest set it. */
static uint16_t win_handle[X86_EMS_WINDOWS];
static uint16_t win_lpage[X86_EMS_WINDOWS];
static uint16_t win_gpage[X86_EMS_WINDOWS];

/* The sixteen fold entries covering the page frame, as they were BEFORE EMS
   took the region: the open-bus filler alias. Restored on unmap so an unmapped
   window reads 0xFF rather than faulting. See x86_ems.h. */
static uint32_t frame_idle_fold[X86_EMS_FRAME_BYTES / 4096u];
/*
 * Have we captured them yet? This is not defensiveness, it is the FIRST-CALL
 * case: `win_lpage` starts as .bss zero, which reads as "logical page 0 is in
 * window 0", so the very first x86_ems_reset() walks the unmap path with
 * frame_idle_fold still all zeroes and would write sixteen fold entries of 0
 * over the page frame. FOLD_K == 0 means host == idx << 12 -- a small positive
 * address that PASSES the interpreter's sentinel check and is then
 * dereferenced, which is the exact hard-fault-instead-of-clean-fault shape
 * x86_map_fixed() refuses a NULL backing to avoid.
 */
static int frame_taken;
static uint32_t ems_pages;          /* logical pages advertised; 0 = absent */

/* ---- results -------------------------------------------------------------
 * INT 67h reports in AH and DOES NOT USE CF. That is not a simplification: the
 * spec has no carry convention, and editing the stacked FLAGS the way an INT
 * 21h service does would make a guest that ignores AH (most of them do not, but
 * some check both) see a success it was never given.
 */
static int ems_ok(void)   { x86_set_ah(X86_EMM_OK); return 1; }
static int ems_err(uint8_t code)
{
    x86_set_ah(code);
    x86_ems_last_err = code;
    return 1;
}

static ems_handle_t *ems_get(uint16_t handle)
{
    if (handle == 0 || handle > X86_EMS_HANDLES)
        return NULL;
    ems_handle_t *h = &ems_h[handle - 1];
    return h->used ? h : NULL;
}

static void ems_account(void)
{
    uint32_t used = 0, n = 0;
    for (unsigned i = 0; i < X86_EMS_HANDLES; i++)
        if (ems_h[i].used) { used += ems_h[i].count; n++; }
    x86_ems_used_pages = used;
    x86_ems_handles_used = n;
    x86_ems_total_pages = ems_pages;
}

/* First fit over the live handles, in logical pages. Sixteen handles, so
   walking them beats any structure that would keep them sorted -- the same
   trade x86_dos.c's XMS allocator makes for the same reason. */
#define EMS_NO_OFF 0xFFFFFFFFu
static uint32_t ems_find_gap(uint32_t count)
{
    uint32_t cand = 0;
    for (;;) {
        if (cand > ems_pages || ems_pages - cand < count)
            return EMS_NO_OFF;
        uint32_t bump = 0;
        for (unsigned i = 0; i < X86_EMS_HANDLES; i++) {
            if (!ems_h[i].used)
                continue;
            uint32_t s = ems_h[i].first, e = s + ems_h[i].count;
            if (cand < e && s < cand + count && e > bump)
                bump = e;
        }
        if (!bump)
            return cand;
        cand = bump;
    }
}

static uint32_t ems_free_pages(void)
{
    uint32_t used = 0;
    for (unsigned i = 0; i < X86_EMS_HANDLES; i++)
        if (ems_h[i].used)
            used += ems_h[i].count;
    return ems_pages - used;
}

/* ---- the mapping itself, which is the whole interface -------------------
 *
 * THE ORDER OF THE THREE STEPS IS THE CORRECTNESS ARGUMENT. Fault the granule
 * in, PIN IT IMMEDIATELY, and only then install the window's fold entry -- and
 * do that per granule rather than in three passes. Faulting granule 2 can evict
 * granule 1 when the pool is tight, and a window entry installed before the pin
 * would then point at whatever page took the frame, with nothing anywhere
 * saying so. That is the exact shape of the eviction bug x86_pager.h records as
 * "5 evictions producing 26,329 wrong bytes with cow_lost == 0 throughout".
 */
static void ems_unmap_window(uint32_t w)
{
    if (win_lpage[w] == EMS_NO_PAGE)
        return;
    uint32_t back = X86_EMS_BACK_PAGE + win_gpage[w] * X86_EMS_SUBPAGES;
    for (uint32_t s = 0; s < X86_EMS_SUBPAGES; s++) {
        x86_unpin_pages(back + s, 1);
        if (x86_ems_pinned_pages)
            x86_ems_pinned_pages--;
        /* Back to the open-bus filler. NOT FOLD_INVALID: an unmapped EMS window
           on real hardware reads whatever the board drives, and a guest that
           reads one -- a scan looking for the frame does exactly that -- must
           get 0xFF and not a memory fault. */
        if (frame_taken)
            guest_page_table_ptr[(X86_EMS_FRAME_LIN >> 12)
                                 + w * X86_EMS_SUBPAGES + s]
                = frame_idle_fold[w * X86_EMS_SUBPAGES + s];
    }
    win_handle[w] = 0;
    win_lpage[w] = EMS_NO_PAGE;
    win_gpage[w] = 0;
}

static int ems_map_window(uint32_t w, uint16_t handle, uint16_t lpage,
                          uint16_t gpage)
{
    ems_unmap_window(w);

    uint32_t back = X86_EMS_BACK_PAGE + (uint32_t)gpage * X86_EMS_SUBPAGES;
    uint32_t first = (X86_EMS_FRAME_LIN >> 12) + w * X86_EMS_SUBPAGES;

    for (uint32_t s = 0; s < X86_EMS_SUBPAGES; s++) {
        uint8_t *host = (uint8_t *)x86_guest_ptr((back + s) << 12, 4096u);
        if (!host) {
            /* Could not back it. Unwind what this call already pinned so a
               refusal costs nothing permanent, and say so LOUDLY -- a half
               mapped window that reported success is the failure this project
               keeps hitting. */
            for (uint32_t u = 0; u < s; u++) {
                x86_unpin_pages(back + u, 1);
                if (x86_ems_pinned_pages)
                    x86_ems_pinned_pages--;
                guest_page_table_ptr[first + u] =
                    frame_idle_fold[w * X86_EMS_SUBPAGES + u];
            }
            x86_ems_map_fails++;
            return -1;
        }
        x86_pin_pages(back + s, 1);
        x86_ems_pinned_pages++;
        guest_page_table_ptr[first + s] = FOLD_K(host, first + s);
    }
    /* The live-pointer ring saw four x86_guest_ptr() calls above. Those
       pointers do not outlive this function -- the fold entries do, and they
       are protected by the PIN, which is durable where the ring is not. */
    win_handle[w] = handle;
    win_lpage[w] = lpage;
    win_gpage[w] = gpage;
    return 0;
}

/* ---- bring-up ----------------------------------------------------------- */

void x86_ems_reset(void)
{
    for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++)
        ems_unmap_window(w);
    for (unsigned i = 0; i < X86_EMS_HANDLES; i++) {
        ems_h[i].used = 0;
        ems_h[i].saved = 0;
        ems_h[i].first = 0;
        ems_h[i].count = 0;
    }
    x86_ems_pinned_pages = 0;
    ems_account();
}

int x86_ems_present(void) { return x86_ems_present_v ? 1 : 0; }

/*
 * The device header and the INT 67h stub, planted only when EMS is real.
 *
 * Two detection routes, because titles use both: DOS's own -- open the
 * character device EMMXXXX0 with INT 21h AH=3Dh, which x86_dos.c answers -- and
 * the direct one, which reads the INT 67h vector, takes its SEGMENT and
 * compares the eight bytes at offset 0x0A against the name. Both are answered
 * by the same eighteen bytes, and neither is answered at all when there is no
 * backing store.
 */
static int ems_install_entry(void)
{
    uint8_t *hdr = (uint8_t *)x86_guest_ptr(X86_EMS_HDR_LIN, 0x24u);
    uint8_t *ivt = (uint8_t *)x86_guest_ptr(0x67u * 4u, 4u);
    if (!hdr || !ivt)
        return -1;

    /* A DOS character-device header. Only the name is ever read by a probe,
       but a plausible header costs eighteen bytes and an implausible one is
       exactly the kind of thing a defensive loader rejects. */
    hdr[0] = 0xFF; hdr[1] = 0xFF; hdr[2] = 0xFF; hdr[3] = 0xFF; /* next: none */
    hdr[4] = 0x00; hdr[5] = 0x80;            /* attribute: character device  */
    hdr[6] = 0x00; hdr[7] = 0x00;            /* strategy   -- never called   */
    hdr[8] = 0x00; hdr[9] = 0x00;            /* interrupt  -- never called   */
    memcpy(hdr + 0x0A, "EMMXXXX0", 8);

    /* 0F 0B 67 IRET, at 0064:0020. Ends 0xCF and not 0xCB: INT 67h is entered
       by an interrupt, unlike the XMS control entry it shares a block with. */
    hdr[X86_EMS_ENTRY_OFF - X86_EMS_HDR_OFF + 0] = 0x0F;
    hdr[X86_EMS_ENTRY_OFF - X86_EMS_HDR_OFF + 1] = 0x0B;
    hdr[X86_EMS_ENTRY_OFF - X86_EMS_HDR_OFF + 2] = 0x67;
    hdr[X86_EMS_ENTRY_OFF - X86_EMS_HDR_OFF + 3] = 0xCF;

    ivt[0] = (uint8_t)X86_EMS_ENTRY_OFF;
    ivt[1] = (uint8_t)(X86_EMS_ENTRY_OFF >> 8);
    ivt[2] = (uint8_t)X86_EMS_ENTRY_SEG;
    ivt[3] = (uint8_t)(X86_EMS_ENTRY_SEG >> 8);
    return 0;
}

int x86_ems_set_backing(uint32_t pages)
{
    x86_ems_present_v = 0;
    x86_ems_frame_refused = 0;
    ems_pages = 0;
    x86_ems_reset();
    frame_taken = 0;

    if (pages == 0)
        return -1;                  /* honestly absent; see x86_ems.h */
    if (pages > X86_EMS_PAGES)
        pages = X86_EMS_PAGES;

    /*
     * IS THE PAGE FRAME ACTUALLY FREE? Checked, not assumed, and the check is
     * the aliasing property itself: the whole upper megabyte except VRAM, the
     * 13h aperture and the BIOS id page is ONE 4 KB page of 0xFF mapped sixteen
     * ways over each 64 KB, so a free window is sixteen fold entries that are
     * mapped and all resolve to the SAME host address. VRAM or the aperture
     * straying into this region would each resolve somewhere of their own and
     * fail this test rather than silently losing the guest's display.
     */
    uint32_t idx0 = X86_EMS_FRAME_LIN >> 12;
    uint32_t host0 = 0;
    for (uint32_t i = 0; i < X86_EMS_FRAME_BYTES / 4096u; i++) {
        uint32_t k = guest_page_table_ptr[idx0 + i];
        if (k == FOLD_INVALID) {
            x86_ems_frame_refused = 1;
            return -1;
        }
        uint32_t host = k + ((idx0 + i) << 12);
        if (i == 0)
            host0 = host;
        else if (host != host0) {
            x86_ems_frame_refused = 1;
            return -1;
        }
        frame_idle_fold[i] = k;
    }
    frame_taken = 1;

    if (ems_install_entry() != 0) {
        x86_ems_frame_refused = 1;
        return -1;
    }

    ems_pages = pages;
    x86_ems_present_v = 1;
    ems_account();
    return 0;
}

/* ---- AH=4Eh's page map --------------------------------------------------
 * The format is DRIVER DEFINED -- the spec says only that AL=03h reports its
 * size and that a buffer produced by 4Eh/00h is accepted by 4Eh/01h. Four
 * windows, each a handle word and a logical-page word: sixteen bytes.
 */
#define EMS_MAP_BYTES  (4u * X86_EMS_WINDOWS)

static int ems_map_save_to(uint32_t lin)
{
    uint8_t buf[EMS_MAP_BYTES];
    for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++) {
        buf[w * 4 + 0] = (uint8_t)win_handle[w];
        buf[w * 4 + 1] = (uint8_t)(win_handle[w] >> 8);
        buf[w * 4 + 2] = (uint8_t)win_lpage[w];
        buf[w * 4 + 3] = (uint8_t)(win_lpage[w] >> 8);
    }
    return x86_guest_copy_in(lin, buf, sizeof(buf)) == sizeof(buf) ? 0 : -1;
}

static int ems_map_apply(const uint16_t *hnd, const uint16_t *lpg)
{
    /* Two passes, and the first one is not optional: a restore that mapped as
       it validated would leave the windows half-way through the OLD map and
       half-way through the new one if the fifth word turned out to be wrong. */
    for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++) {
        if (lpg[w] == EMS_NO_PAGE)
            continue;
        ems_handle_t *h = ems_get(hnd[w]);
        if (!h)
            return X86_EMM_BAD_HANDLE;
        if (lpg[w] >= h->count)
            return X86_EMM_BAD_LOGICAL;
    }
    for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++) {
        if (lpg[w] == EMS_NO_PAGE) {
            ems_unmap_window(w);
            continue;
        }
        ems_handle_t *h = ems_get(hnd[w]);
        if (ems_map_window(w, hnd[w], lpg[w],
                           (uint16_t)(h->first + lpg[w])) != 0)
            return X86_EMM_SOFT_ERR;
    }
    return X86_EMM_OK;
}

static int ems_map_load_from(uint32_t lin)
{
    uint8_t buf[EMS_MAP_BYTES];
    uint16_t hnd[X86_EMS_WINDOWS], lpg[X86_EMS_WINDOWS];
    if (x86_guest_copy_out(buf, lin, sizeof(buf)) != sizeof(buf))
        return X86_EMM_SOFT_ERR;
    for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++) {
        hnd[w] = (uint16_t)(buf[w * 4 + 0] | (buf[w * 4 + 1] << 8));
        lpg[w] = (uint16_t)(buf[w * 4 + 2] | (buf[w * 4 + 3] << 8));
    }
    return ems_map_apply(hnd, lpg);
}

/* ---- the service --------------------------------------------------------- */

int x86_ems_int67(void)
{
    uint8_t func = (uint8_t)(cpu_state_ptr->regs[0] >> 8);
    uint8_t al   = (uint8_t)cpu_state_ptr->regs[0];
    uint16_t bx  = (uint16_t)cpu_state_ptr->regs[3];
    uint16_t dx  = (uint16_t)cpu_state_ptr->regs[2];

    x86_ems_calls++;
    if (!x86_ems_present_v)
        return ems_err(X86_EMM_HARD_ERR);

    switch (func) {
    case 0x40:                                  /* get manager status */
        return ems_ok();

    case 0x41:                                  /* get page frame segment */
        x86_set_bx(X86_EMS_FRAME_SEG);
        return ems_ok();

    case 0x42:                                  /* get unallocated / total */
        x86_set_bx((uint16_t)ems_free_pages());
        x86_set_dx((uint16_t)ems_pages);
        return ems_ok();

    case 0x43: {                                /* allocate pages */
        if (bx == 0)
            return ems_err(X86_EMM_ZERO_PAGES);
        if (bx > ems_pages)
            return ems_err(X86_EMM_TOO_MANY);
        unsigned i;
        for (i = 0; i < X86_EMS_HANDLES; i++)
            if (!ems_h[i].used)
                break;
        if (i == X86_EMS_HANDLES)
            return ems_err(X86_EMM_NO_HANDLES);
        uint32_t off = ems_find_gap(bx);
        if (off == EMS_NO_OFF)
            return ems_err(X86_EMM_NO_PAGES);
        ems_h[i].used = 1;
        ems_h[i].saved = 0;
        ems_h[i].first = (uint16_t)off;
        ems_h[i].count = bx;
        ems_account();
        x86_set_dx((uint16_t)(i + 1));
        return ems_ok();
    }

    case 0x44: {                                /* map a page into a window */
        if (al >= X86_EMS_WINDOWS)
            return ems_err(X86_EMM_BAD_PHYSICAL);
        ems_handle_t *h = ems_get(dx);
        if (!h)
            return ems_err(X86_EMM_BAD_HANDLE);
        if (bx == EMS_NO_PAGE) {                /* the documented unmap */
            ems_unmap_window(al);
            x86_ems_unmaps++;
            return ems_ok();
        }
        if (bx >= h->count)
            return ems_err(X86_EMM_BAD_LOGICAL);
        if (ems_map_window(al, dx, bx, (uint16_t)(h->first + bx)) != 0)
            return ems_err(X86_EMM_SOFT_ERR);
        x86_ems_maps++;
        return ems_ok();
    }

    case 0x45: {                                /* deallocate pages */
        ems_handle_t *h = ems_get(dx);
        if (!h)
            return ems_err(X86_EMM_BAD_HANDLE);
        if (h->saved)
            return ems_err(X86_EMM_SAVE_STATE);
        /* Any window still showing this handle's memory has to let go of its
           pins FIRST -- otherwise the pages stay pinned for the life of the
           title and the pool loses four frames per leaked handle. */
        for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++)
            if (win_lpage[w] != EMS_NO_PAGE && win_handle[w] == dx)
                ems_unmap_window(w);
        h->used = 0;
        h->count = 0;
        ems_account();
        return ems_ok();
    }

    case 0x46:                                  /* get version */
        /* 3.2 in BCD, and the function set below IS the 3.2 set. See
           x86_ems.h for why claiming 4.0 would be worse than useless. */
        x86_set_al(0x32);
        return ems_ok();

    case 0x47: {                                /* save page map */
        ems_handle_t *h = ems_get(dx);
        if (!h)
            return ems_err(X86_EMM_BAD_HANDLE);
        if (h->saved)
            return ems_err(X86_EMM_SAVE_DONE);
        for (uint32_t w = 0; w < X86_EMS_WINDOWS; w++) {
            h->save_h[w] = win_handle[w];
            h->save_l[w] = win_lpage[w];
        }
        h->saved = 1;
        return ems_ok();
    }

    case 0x48: {                                /* restore page map */
        ems_handle_t *h = ems_get(dx);
        if (!h)
            return ems_err(X86_EMM_BAD_HANDLE);
        if (!h->saved)
            return ems_err(X86_EMM_NO_SAVE);
        uint8_t rc = (uint8_t)ems_map_apply(h->save_h, h->save_l);
        h->saved = 0;
        return rc == X86_EMM_OK ? ems_ok() : ems_err(rc);
    }

    case 0x4B:                                  /* get handle count */
        /* Handle 0 is the driver's own and is never issued, so it counts. */
        x86_set_bx((uint16_t)(x86_ems_handles_used + 1u));
        return ems_ok();

    case 0x4C: {                                /* pages owned by a handle */
        ems_handle_t *h = ems_get(dx);
        if (!h)
            return ems_err(X86_EMM_BAD_HANDLE);
        x86_set_bx(h->count);
        return ems_ok();
    }

    case 0x4D: {                                /* pages owned by ALL handles */
        uint32_t lin = cpu_state_ptr->segs[0].base
                       + (cpu_state_ptr->regs[7] & 0xFFFFu);   /* ES:DI */
        uint16_t n = 0;
        for (unsigned i = 0; i < X86_EMS_HANDLES; i++) {
            uint8_t e[4];
            if (!ems_h[i].used)
                continue;
            e[0] = (uint8_t)(i + 1); e[1] = 0;
            e[2] = (uint8_t)ems_h[i].count;
            e[3] = (uint8_t)(ems_h[i].count >> 8);
            if (x86_guest_copy_in(lin + n * 4u, e, 4) != 4)
                return ems_err(X86_EMM_SOFT_ERR);
            n++;
        }
        x86_set_bx(n);
        return ems_ok();
    }

    case 0x4E:                                  /* get / set the page map */
        switch (al) {
        case 0x00: {                            /* get, to ES:DI */
            uint32_t lin = cpu_state_ptr->segs[0].base
                           + (cpu_state_ptr->regs[7] & 0xFFFFu);
            if (ems_map_save_to(lin) != 0)
                return ems_err(X86_EMM_SOFT_ERR);
            return ems_ok();
        }
        case 0x01: {                            /* set, from DS:SI */
            uint32_t lin = cpu_state_ptr->segs[3].base
                           + (cpu_state_ptr->regs[6] & 0xFFFFu);
            uint8_t rc = (uint8_t)ems_map_load_from(lin);
            return rc == X86_EMM_OK ? ems_ok() : ems_err(rc);
        }
        case 0x02: {                            /* get to ES:DI, set from DS:SI */
            uint32_t dst = cpu_state_ptr->segs[0].base
                           + (cpu_state_ptr->regs[7] & 0xFFFFu);
            uint32_t src = cpu_state_ptr->segs[3].base
                           + (cpu_state_ptr->regs[6] & 0xFFFFu);
            if (ems_map_save_to(dst) != 0)
                return ems_err(X86_EMM_SOFT_ERR);
            uint8_t rc = (uint8_t)ems_map_load_from(src);
            return rc == X86_EMM_OK ? ems_ok() : ems_err(rc);
        }
        case 0x03:                              /* size of the map, in bytes */
            x86_set_al(EMS_MAP_BYTES);
            return ems_ok();
        default:
            x86_ems_unhandled = 0x4E00u | al;
            return ems_err(X86_EMM_BAD_SUBFUNC);
        }

    default:
        /* Loud, like every other unimplemented service in this tree. A 4.0
           call landing here is the signal that AH=46h's 3.2 was ignored. */
        x86_ems_unhandled = func;
        return ems_err(X86_EMM_BAD_FUNC);
    }
}
