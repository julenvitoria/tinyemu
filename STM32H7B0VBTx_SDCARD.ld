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

  ._itcram_hot : {
    . = ALIGN(4);
    *(.itcram_hot)
    *(.itcram_hot*)
    . = ALIGN(4);
  } > ITCMRAM

  .dtcm_bss (NOLOAD) : {
    . = ALIGN(4);
    *(.dtcm_bss)
    *(.dtcm_bss*)
    . = ALIGN(4);
  } > DTCMRAM
}
