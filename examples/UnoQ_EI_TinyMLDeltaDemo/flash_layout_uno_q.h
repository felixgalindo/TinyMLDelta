/*
 * Flash Layout — Arduino UNO R4 (RAM-backed demo)
 * ------------------------------------------------
 * Shared with the arduino_uno_q_tflm example.
 * See that example for full documentation.
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#pragma once
#include <stdint.h>
#include "tinymldelta_ports.h"

#define TMD_SLOT_BYTES   4096u
#define TMD_META_BYTES    256u

#define TMD_SLOT_A_ADDR  0x0000u
#define TMD_SLOT_B_ADDR  0x1000u
#define TMD_META_ADDR    0x2000u

static const tmd_layout_t g_uno_q_layout = {
    .slotA     = { .addr = TMD_SLOT_A_ADDR, .size = TMD_SLOT_BYTES },
    .slotB     = { .addr = TMD_SLOT_B_ADDR, .size = TMD_SLOT_BYTES },
    .meta_addr = TMD_META_ADDR,
    .meta_size = TMD_META_BYTES,
};
