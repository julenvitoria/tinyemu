/* 
 * Linker script for STM32H7B0VB (SD Card Variant)
 * Mirrors the Retro-Go SD Card memory layout for TinyEMU bare-metal,
 * heavily pruned to just the essentials for this core.
 */

ENTRY(Reset_Handler)

MEMORY {
  ITCMRAM  (xrw) : ORIGIN = 0x00000000, LENGTH = 64K
  DTCMRAM  (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  
  /* AXI SRAM (1024K total). Reclaiming LCD pool (154K) leaves 870K for emulator */
  RAM_UC   (xrw) : ORIGIN = 0x24000000, LENGTH = 154K
  RAM_EMU  (xrw) : ORIGIN = 0x24000000 + 154K, LENGTH = 870K
  
  AHBRAM   (xrw) : ORIGIN = 0x30000000, LENGTH = 128K
  
  INTFLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  EXTFLASH (rx)  : ORIGIN = 0x90000000, LENGTH = 32M
}

SECTIONS {
  .isr_vector : {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > INTFLASH

  .text : {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > INTFLASH

  _sidata = LOADADDR(.data);
  .data : {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM_EMU AT> INTFLASH

  .bss : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM_EMU

  /* Hot execution loop runs from 0-wait-state ITCM. Stored in flash, copied
     in by Reset_Handler before anything branches into it. Mirrors
     STM32H7B0VBTx_SDCARD.ld, which has always placed this correctly. */
  _sitcm_load = LOADADDR(._itcram_hot);
  ._itcram_hot : {
    . = ALIGN(4);
    _sitcm = .;
    *(.itcram_hot)
    *(.itcram_hot*)
    . = ALIGN(4);
    _eitcm = .;
  } > ITCMRAM AT> INTFLASH

  /* CPU state and the guest page table must be in 0-wait-state DTCM.
     This previously landed in RAM_EMU (AXI SRAM), so every page-table walk
     paid AXI latency and no measurement taken before this fix is valid. */
  .dtcm_bss (NOLOAD) : {
    . = ALIGN(4);
    *(.dtcm_bss)
    *(.dtcm_bss*)
    . = ALIGN(4);
  } > DTCMRAM

  /* Harness-only scratch: the video test's VRAM, its render target and its
     palette. These are the HOST's copies, not guest memory, and they were
     costing 107 KB of RAM_EMU -- which left 2,948 bytes of headroom and made
     any new object in front of the 4 KB-aligned test_ram overflow the region
     by a whole page. The bare-metal harness drives no LCD, so RAM_UC (the
     firmware's framebuffer pool) is dead space here and is exactly where this
     belongs. Zeroed explicitly by Reset_Handler; it is outside _sbss.._ebss. */
  .test_bss (NOLOAD) : {
    . = ALIGN(4);
    _stestbss = .;
    *(.test_bss)
    *(.test_bss*)
    . = ALIGN(4);
    _etestbss = .;
  } > RAM_UC

  /* Mode 13h's 64 KB aperture, its palette block and the DAC scratch. AHBRAM
     is 128 KB and the bare-metal harness uses none of it, while RAM_UC has
     ~48 KB free and RAM_EMU ~20 KB -- neither fits 67 KB on its own, and
     splitting the aperture from its palette to make it fit would be a layout
     the real core does not have. In the real core these come from the LCD
     bonus pool; here they only have to exist and be host-readable.
     Zeroed by Reset_Handler alongside .test_bss. */
  .ahb_bss (NOLOAD) : {
    . = ALIGN(4);
    _sahbbss = .;
    *(.ahb_bss)
    *(.ahb_bss*)
    . = ALIGN(4);
    _eahbbss = .;
  } > AHBRAM
}
