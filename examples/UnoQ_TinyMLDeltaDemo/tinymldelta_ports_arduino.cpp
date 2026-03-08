/*
 * TinyMLDelta — Arduino UNO R4 Port (RAM-backed flash demo)
 * ----------------------------------------------------------
 * Implements the TinyMLDelta platform ports using two RAM arrays as the
 * A/B model slots. This is the correct approach for the UNO R4's 256 KB
 * internal flash, where live in-place flash rewriting of firmware pages
 * is not practical without a full bootloader integration.
 *
 * For production use on a device with external SPI/QSPI flash, replace
 * the ram_flash_* functions with calls to your flash driver.
 *
 * RAM budget (UNO R4: 32 KB SRAM):
 *   g_slotA_ram   4096 B   model slot A
 *   g_slotB_ram   4096 B   model slot B
 *   g_meta_ram     256 B   TinyMLDelta journal
 *   (tensor arena, patch buf, stack — rest of 32 KB used by sketch)
 *
 * Public API for the sketch:
 *   tmd_arduino_init_slots(base, len)  — copy base model into slot A
 *   tmd_arduino_active_slot_ptr()      — pointer to active model bytes
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <string.h>
#include "tinymldelta_ports.h"
#include "tinymldelta_config.h"
#include "flash_layout_uno_q.h"

/* =========================================================================
 * RAM-backed flash storage
 *
 * The virtual flash address space is:
 *   [0x0000 .. 0x0FFF]  slot A  (TMD_SLOT_BYTES = 4096)
 *   [0x1000 .. 0x1FFF]  slot B
 *   [0x2000 .. 0x20FF]  meta / journal (TMD_META_BYTES = 256)
 * ====================================================================== */

uint8_t g_slotA_ram[TMD_SLOT_BYTES];
uint8_t g_slotB_ram[TMD_SLOT_BYTES];
uint8_t g_meta_ram[TMD_META_BYTES];

static uint8_t g_active_slot = 0;   /* 0 = slot A, 1 = slot B */

/* Map a virtual address + len to the correct RAM region.
 * Returns a pointer to the start of the region, or nullptr on out-of-range. */
static uint8_t* ram_ptr(uint32_t addr, uint32_t len) {
    uint32_t slotA_end = TMD_SLOT_A_ADDR + TMD_SLOT_BYTES;
    uint32_t slotB_end = TMD_SLOT_B_ADDR + TMD_SLOT_BYTES;
    uint32_t meta_end  = TMD_META_ADDR   + TMD_META_BYTES;

    if (addr >= TMD_SLOT_A_ADDR && addr + len <= slotA_end) {
        return g_slotA_ram + (addr - TMD_SLOT_A_ADDR);
    }
    if (addr >= TMD_SLOT_B_ADDR && addr + len <= slotB_end) {
        return g_slotB_ram + (addr - TMD_SLOT_B_ADDR);
    }
    if (addr >= TMD_META_ADDR && addr + len <= meta_end) {
        return g_meta_ram + (addr - TMD_META_ADDR);
    }
    return nullptr;
}

/* =========================================================================
 * Flash primitives
 * ====================================================================== */

static bool ram_flash_erase(uint32_t addr, uint32_t len) {
    uint8_t* p = ram_ptr(addr, len);
    if (!p) return false;
    memset(p, 0xFF, len);
    return true;
}

static bool ram_flash_write(uint32_t addr, const void* src, uint32_t len) {
    uint8_t* p = ram_ptr(addr, len);
    if (!p || !src) return false;
    memcpy(p, src, len);
    return true;
}

static bool ram_flash_read(uint32_t addr, void* dst, uint32_t len) {
    const uint8_t* p = ram_ptr(addr, len);
    if (!p || !dst) return false;
    memcpy(dst, p, len);
    return true;
}

/* =========================================================================
 * CRC32 (software, polynomial 0xEDB88320)
 * ====================================================================== */

#if TMD_FEAT_CRC32
static uint32_t crc32_sw(const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
#endif

/* =========================================================================
 * Active slot tracking
 * ====================================================================== */

static uint8_t arduino_get_active_slot(void) {
    return g_active_slot;
}

static bool arduino_set_active_slot(uint8_t idx) {
    if (idx > 1) return false;
    g_active_slot = idx;
    return true;
}

/* =========================================================================
 * Journal (stored in g_meta_ram)
 * ====================================================================== */

#if TMD_FEAT_JOURNAL
static bool arduino_journal_read(tmd_journal_t* out) {
    if (!out) return false;
    return ram_flash_read(TMD_META_ADDR, out, sizeof(*out));
}

static bool arduino_journal_write(const tmd_journal_t* in) {
    if (!in) return false;
    ram_flash_erase(TMD_META_ADDR, TMD_META_BYTES);
    return ram_flash_write(TMD_META_ADDR, in, sizeof(*in));
}

static bool arduino_journal_clear(void) {
    tmd_journal_t empty;
    memset(&empty, 0, sizeof(empty));
    return arduino_journal_write(&empty);
}
#endif

/* =========================================================================
 * Logging
 * ====================================================================== */

#if TMD_FEAT_LOG
#include <stdarg.h>
static void arduino_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}
#endif

/* =========================================================================
 * Port table + layout
 * ====================================================================== */

static tmd_ports_t g_ports = {
    .flash_erase     = ram_flash_erase,
    .flash_write     = ram_flash_write,
    .flash_read      = ram_flash_read,
#if TMD_FEAT_CRC32
    .crc32           = crc32_sw,
#endif
#if TMD_FEAT_SHA256
    .sha256_init     = nullptr,
    .sha256_update   = nullptr,
    .sha256_final    = nullptr,
#endif
#if TMD_FEAT_AES_CMAC
    .cmac_verify     = nullptr,
#endif
    .get_active_slot = arduino_get_active_slot,
    .set_active_slot = arduino_set_active_slot,
#if TMD_FEAT_JOURNAL
    .journal_read    = arduino_journal_read,
    .journal_write   = arduino_journal_write,
    .journal_clear   = arduino_journal_clear,
#endif
#if TMD_FEAT_LOG
    .log             = arduino_log,
#endif
};

extern "C" {

const tmd_ports_t* tmd_ports(void) {
    return &g_ports;
}

const tmd_layout_t* tmd_layout(void) {
    return &g_uno_q_layout;
}

} /* extern "C" */

/* =========================================================================
 * Public helpers called by UnoQ_TinyMLDeltaDemo.ino
 * ====================================================================== */

/*
 * tmd_arduino_init_slots(base, len)
 * ---------------------------------
 * Copies the base model bytes into slot A and fills slot B with 0xFF.
 * Resets the active slot to 0 (slot A).
 * Call this once during setup() before any patch is applied.
 */
void tmd_arduino_init_slots(const uint8_t* base, size_t len) {
    memset(g_slotA_ram, 0xFF, TMD_SLOT_BYTES);
    memset(g_slotB_ram, 0xFF, TMD_SLOT_BYTES);
    memset(g_meta_ram,  0x00, TMD_META_BYTES);

    if (base && len > 0) {
        size_t copy_len = (len < (size_t)TMD_SLOT_BYTES) ? len : (size_t)TMD_SLOT_BYTES;
        memcpy(g_slotA_ram, base, copy_len);
    }

    g_active_slot = 0;
}

/*
 * tmd_arduino_active_slot_ptr()
 * ------------------------------
 * Returns a direct pointer to the first byte of the currently active
 * model slot (either g_slotA_ram or g_slotB_ram).
 *
 * This pointer is valid as long as no patch is being applied. After
 * tmd_apply_patch_from_memory() returns, call this again to get the
 * pointer to the newly activated slot.
 */
const uint8_t* tmd_arduino_active_slot_ptr(void) {
    return (g_active_slot == 0) ? g_slotA_ram : g_slotB_ram;
}
