/*
 * Linker script — TinyMLDelta STM32L4 example, STM32L476xG (SensorTile: L476JG).
 *
 * Standalone image at 0x08000000: no ST bootloader, no FOTA staging, no vector
 * table offset games. The reset vector therefore points into the first pages of
 * flash, which is exactly what st-flash expects at that address.
 *
 * FLASH is deliberately capped at 384K rather than the full 1M. Everything from
 * 0x08060000 up belongs to the TinyMLDelta regions declared in flash_layout.h,
 * and the cap makes the linker — not a runtime surprise — the thing that catches
 * an application that grows into the model slots.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

ENTRY(Reset_Handler)

_estack = 0x20018000;              /* top of the 96K SRAM1 */

_Min_Heap_Size  = 0x0;             /* no malloc in this build */
_Min_Stack_Size = 0x1000;          /* 4K */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 384K   /* app only; slots start at 0x08060000 */
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
  SRAM2 (xrw) : ORIGIN = 0x10000000, LENGTH = 32K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(8);
    KEEP(*(.isr_vector))
    . = ALIGN(8);
  } >FLASH

  .text :
  {
    . = ALIGN(8);
    *(.text) *(.text*)
    *(.glue_7) *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(8);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(8);
    *(.rodata) *(.rodata*)
    . = ALIGN(8);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array    : { PROVIDE_HIDDEN(__init_array_start = .);    KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  ASSERT(__init_array_end == __init_array_start, "C++ constructors found: board.c runs no .init_array")
  .fini_array    : { PROVIDE_HIDDEN(__fini_array_start = .);    KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(8);
    _sdata = .;
    *(.data) *(.data*)
    . = ALIGN(8);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(8);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss) *(.bss*)
    *(COMMON)
    . = ALIGN(8);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* Survives reset and is NOT cleared by startup — this is where the status
   * block and the log ring buffer live, so the host can read them back over SWD
   * after the firmware has finished running. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(8);
    _snoinit = .;
    *(.noinit) *(.noinit*)
    . = ALIGN(8);
    _enoinit = .;
  } >RAM

  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* No /DISCARD/ of libc.a / libm.a here: this build links -nostdlib, so those
   * archives do not exist and naming them makes ld fail looking for them. */

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
