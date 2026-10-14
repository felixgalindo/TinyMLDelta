/**
 * @file flash_layout.h
 * @brief TinyMLDelta flash map for the SensorTile (STM32L476JG).
 *
 * -----------------------------------------------------------------------------
 * STM32L476xG geometry (same for the JG on the SensorTile and the RG on a Nucleo)
 * -----------------------------------------------------------------------------
 *   1 MiB flash, DUAL BANK (the factory default for the 1 MiB part):
 *       bank 1 : 0x08000000 .. 0x0807FFFF   pages   0..255
 *       bank 2 : 0x08080000 .. 0x080FFFFF   pages   0..255
 *   Page size     : 2 KiB  (smallest erasable unit)
 *   Write unit    : 64-bit DOUBLEWORD, 8-byte aligned, ERASED (0xFF) target only
 *
 * That write rule is the whole reason tmd_port_stm32l4.c exists: the TinyMLDelta
 * core writes chunks of arbitrary offset and length (3 bytes at an odd address is
 * normal), which real NOR flash cannot do. The port buffers a full page in RAM,
 * read-modify-writes it, then erases and reprograms.
 *
 * -----------------------------------------------------------------------------
 * Map — everything lives in bank 1, far above the application
 * -----------------------------------------------------------------------------
 *
 *   +--------------------------+ 0x08000000  page   0
 *   | application              |  384 KiB reserved (we use well under 64 KiB)
 *   +--------------------------+ 0x08060000  page 192
 *   | SLOT A   (model)         |   32 KiB    16 pages
 *   +--------------------------+ 0x08068000  page 208
 *   | SLOT B   (model)         |   32 KiB    16 pages
 *   +--------------------------+ 0x08070000  page 224
 *   | journal                  |    2 KiB     1 page   (append-only records)
 *   +--------------------------+ 0x08070800  page 225
 *   | active-slot marker       |    2 KiB     1 page
 *   +--------------------------+ 0x08071000  page 226
 *   | (unused)                 |   28 KiB
 *   +--------------------------+ 0x08078000  page 240
 *   | patch staging            |   32 KiB    16 pages  (host writes .tmd here)
 *   +--------------------------+ 0x08080000  end of bank 1
 *
 * Slot size is 32 KiB against a 21,560-byte model — room to grow without
 * needing to move anything.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#ifndef FLASH_LAYOUT_H_
#define FLASH_LAYOUT_H_

#include <stdint.h>

/* --- device geometry ------------------------------------------------------ */
#define L4_FLASH_BASE        0x08000000u
#define L4_PAGE_SZ           2048u          /* bank 1 has 256 of these */
#define L4_BANK2_BASE        0x08080000u

/* --- regions (each one a whole number of pages) --------------------------- */
#define TMD_SLOT_BYTES       (32u * 1024u)

#define TMD_SLOT_A_ADDR      0x08060000u    /* the application must end below
                                               this; the linker script enforces
                                               it with FLASH LENGTH = 384K */
#define TMD_SLOT_B_ADDR      0x08068000u

#define TMD_JOURNAL_ADDR     0x08070000u
#define TMD_JOURNAL_BYTES    L4_PAGE_SZ

#define TMD_MARKER_ADDR      0x08070800u    /* one page */

#define TMD_STAGING_ADDR     0x08078000u
#define TMD_STAGING_BYTES    (32u * 1024u)

#endif /* FLASH_LAYOUT_H_ */
