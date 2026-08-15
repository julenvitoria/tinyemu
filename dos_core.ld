/*
 * Retro-Go SD core linker script for the TinyEMU x86 DOS core.
 *
 * Forked from sdk/ld/core_ram_emu.ld per its own "Extending (multi-segment
 * cores)" instructions, to add a GNW_CORE_REGION_ITCM segment. Read that file
 * first -- it is the authoritative reference for the layout, the symbols and
 * the pitfalls; only the deltas are commented here.
 *
 * Reference implementation: cores/pce/pce_core.ld in the firmware tree.
 *
 * Two segments:
 *   0  RAM_EMU  entry trampoline + C code + data + bss   (mandatory, offset 0)
 *   1  ITCM     the hand-written Thumb-2 interpreter hot loop
 *
 * The interpreter's assembly emits into .itcram_hot, which the bare-metal test
 * harness (TEST.ld) maps to ITCMRAM directly. Gathering that same input section
 * into .core_itcm here means ONE assembly source serves both builds -- no
 * per-target section renaming.
 *
 * NOTE: DTCM is deliberately absent. It is not a core load target; it is a
 * firmware bump pool reached via mem_ctl(GW_MEM_DTC, ...) at runtime
 * ("AHB/DTCM are firmware dynamic pools (malloc / dtc_*), not load targets"
 * -- gnw_core_meta.h). The CPU state and guest page table are allocated there
 * at startup and reached through cpu_state_ptr / guest_page_table_ptr.
 *
 * NOTE: taking ITCM as a load segment consumes the same 64K that the firmware
 * otherwise offers as the GW_MEM_ITC bump pool. That is intentional here -- the
 * hot loop is the whole performance thesis -- but it means this core must not
 * also ask for ITC allocations.
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld

MEMORY
{
  RAM_EMU (xrw) : ORIGIN = __RAM_EMU_START__,  LENGTH = __RAM_EMU_LENGTH__
  ITCM    (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
}

PHDRS
{
  text PT_LOAD FLAGS(5); /* PF_R | PF_X */
  data PT_LOAD FLAGS(6); /* PF_R | PF_W */
  itcm PT_LOAD FLAGS(5); /* PF_R | PF_X */
}

SECTIONS
{
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))

    . = ALIGN(4);
    *(.text*)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;
  } >RAM_EMU :text

  /* No ALIGN before this -- .data may be empty, and an ADVANCE without
     CONTENTS desynchronises __CORE_CODE_END__ from the flat binary. */
  .data :
  {
    *(.data*)
  } >RAM_EMU :data

  __CORE_CODE_END__ = .;

  .bss (NOLOAD) :
  {
    . = ALIGN(8);
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } >RAM_EMU :data

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: core code+data+bss overflows RAM_EMU")

  /* ------------------------------------------------------------------ */
  /* Segment 1: the interpreter hot loop, executing from 0-wait ITCM.    */
  /* VMA in ITCM, LMA in RAM_EMU so the payload carries the bytes;       */
  /* pack_core.py auto-detects the __CORE_ITCM_* symbol triple.          */
  /* ------------------------------------------------------------------ */
  .core_itcm __ITCM_CORE_START__ : AT(__CORE_CODE_END__)
  {
    __CORE_ITCM_CODE_START__ = .;
    *(.itcram_hot)
    *(.itcram_hot*)
    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM :itcm

  /* Code-only segment: the interpreter's state lives in DTCM (runtime) and
     RAM_EMU (.bss), never in ITCM. */
  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_CODE_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
         "Error: interpreter hot loop overflows the 64K ITCM segment")

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
