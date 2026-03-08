/*
 * Flash Layout — Arduino UNO R4 (RAM-backed demo)
 * ------------------------------------------------
 * Defines the virtual address space used by the RAM-backed TinyMLDelta
 * port for the Arduino UNO R4 demo.
 *
 * The "flash" here is two SRAM arrays (g_slotA_ram, g_slotB_ram) in
 * tinymldelta_ports_arduino.cpp. Their virtual addresses are mapped by
 * the ram_ptr() helper in that file.
 *
 * To port to real external flash (QSPI, SPI NOR, etc.), adjust the
 * addresses below to match your hardware memory map and replace the
 * ram_flash_* functions with actual driver calls.
 *
 * Virtual memory map:
 *
 *   0x0000 ─── Slot A (4 KB) ─── 0x0FFF
 *   0x1000 ─── Slot B (4 KB) ─── 0x1FFF
 *   0x2000 ─── Meta   (256 B) ── 0x20FF
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#pragma once
#include <stdint.h>
#include "tinymldelta_ports.h"

/* -------------------------------------------------------------------------
 * Geometry constants (used by both the layout struct and the ports file)
 * ---------------------------------------------------------------------- */

#define TMD_SLOT_BYTES   4096u     /* size of each model slot             */
#define TMD_META_BYTES    256u     /* size of journal / meta region        */

#define TMD_SLOT_A_ADDR  0x0000u   /* virtual base address of slot A      */
#define TMD_SLOT_B_ADDR  0x1000u   /* virtual base address of slot B      */
#define TMD_META_ADDR    0x2000u   /* virtual base address of meta region  */

/* -------------------------------------------------------------------------
 * TinyMLDelta layout struct (consumed by tinymldelta_core.c via tmd_layout())
 * ---------------------------------------------------------------------- */

static const tmd_layout_t g_uno_q_layout = {
    .slotA = {
        .addr = TMD_SLOT_A_ADDR,
        .size = TMD_SLOT_BYTES,
    },
    .slotB = {
        .addr = TMD_SLOT_B_ADDR,
        .size = TMD_SLOT_BYTES,
    },
    .meta_addr = TMD_META_ADDR,
    .meta_size = TMD_META_BYTES,
};
