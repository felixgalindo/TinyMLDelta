/**
 * @file flash_layout.h
 * @brief POSIX flash layout for TinyMLDelta dual-slot update demo.
 *
 * This file defines the simulated flash memory map used by the
 * POSIX runtime (flash.bin) during patch testing.
 *
 * Author: Felix Galindo
 * License: Apache-2.0
 *
 * -----------------------------------------------------------------------------
 * FLASH MAP (POSIX DEMO — 256 KiB total)
 * -----------------------------------------------------------------------------
 *
 *   flash.bin total size: 256 KiB (262144 bytes)
 *
 *   +---------------------------+ 0x00000 (0 KiB)
 *   |         Slot A           |
 *   |     128 KiB region       |
 *   |  (base/active firmware)  |
 *   +---------------------------+ 0x20000 (128 KiB)
 *   |         Slot B           |
 *   |     128 KiB region       |
 *   | (inactive/target write)  |
 *   +---------------------------+ 0x40000 (256 KiB)  <-- End of flash
 *
 * Notes:
 *  - This layout matches make_flash.py and run_demo.sh exactly.
 *  - No metadata/manifest region is used in the POSIX demo.
 *  - Real MCU ports will replace this with actual flash geometry.
 *
 * -----------------------------------------------------------------------------
 */

#ifndef FLASH_LAYOUT_H_
#define FLASH_LAYOUT_H_

#include <stdint.h>
#include "tinymldelta_ports.h"

/* -------------------------------------------------------------------------- */
/* Flash Geometry Constants                                                   */
/* -------------------------------------------------------------------------- */

#define TMD_POSIX_FLASH_BYTES   (256u * 1024u)   /* Total flash.bin size       */
#define TMD_POSIX_SLOT_BYTES    (128u * 1024u)   /* Each slot is 128 KiB       */

#define TMD_POSIX_SLOT_A_ADDR   (0u)                       /* Offset 0x00000       */
#define TMD_POSIX_SLOT_B_ADDR   (TMD_POSIX_SLOT_BYTES)     /* Offset 0x20000       */

/* -------------------------------------------------------------------------- */
/* TinyMLDelta Layout Structure                                               */
/* -------------------------------------------------------------------------- */

static const tmd_layout_t g_layout = {
    .slotA = {
        .addr = TMD_POSIX_SLOT_A_ADDR,
        .size = TMD_POSIX_SLOT_BYTES,
    },
    .slotB = {
        .addr = TMD_POSIX_SLOT_B_ADDR,
        .size = TMD_POSIX_SLOT_BYTES,
    },

    /* POSIX demo: no metadata region is used, but fields must exist. */
    .meta_addr = TMD_POSIX_FLASH_BYTES,   /* Disabled */
    .meta_size = 0u,
};

#endif /* FLASH_LAYOUT_H_ */
