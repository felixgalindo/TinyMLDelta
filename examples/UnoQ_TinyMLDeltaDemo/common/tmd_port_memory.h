/*
 * tmd_port_memory.h — In-memory TinyMLDelta flash port for Linux demos.
 *
 * Provides a RAM-backed implementation of the TinyMLDelta platform
 * abstraction (tmd_ports_t / tmd_layout_t).  Used by both the TFLite
 * and Edge Impulse demo apps on the UNO Q Linux co-processor.
 *
 * On real MCU hardware, replace this with actual flash drivers.
 *
 * Usage:
 *   #define TMD_PORT_META_SIZE 256  // optional, default 256
 *   #include "tmd_port_memory.h"
 *
 *   Before calling tmd_apply_patch_from_memory(), set up the layout:
 *     tmd_mem_setup(base_data, base_len, slot_size);
 *   After patching, retrieve the result:
 *     const uint8_t *result = tmd_mem_result(&result_len);
 *     tmd_mem_cleanup();
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#ifndef TMD_PORT_MEMORY_H_
#define TMD_PORT_MEMORY_H_

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

#include "tinymldelta.h"
#include "tinymldelta_config.h"
#include "tinymldelta_ports.h"

#ifndef TMD_PORT_META_SIZE
#define TMD_PORT_META_SIZE 256
#endif

// =============================================================================
// In-memory flash state
//
// Virtual address layout (per tmd_mem_setup() call):
//   [0            .. slot_size-1]   Slot A  (base model, read)
//   [slot_size    .. 2*slot_size-1] Slot B  (receives patched model)
//   [2*slot_size  .. +meta_size-1]  Journal
// =============================================================================

static uint8_t    *g_slot_a    = nullptr;
static uint8_t    *g_slot_b    = nullptr;
static uint8_t     g_meta[TMD_PORT_META_SIZE];
static size_t      g_slot_size = 0;
static uint8_t     g_active    = 0;
static tmd_layout_t g_layout   = {};

// Backing storage (managed by setup/cleanup)
static std::vector<uint8_t> g_slot_a_buf;
static std::vector<uint8_t> g_slot_b_buf;

// =============================================================================
// Setup / result / cleanup helpers
// =============================================================================

/*
 * Prepare the in-memory port for a patch apply operation.
 * Copies base_data into Slot A and initializes the layout.
 */
static void tmd_mem_setup(const uint8_t *base_data, size_t base_len, size_t slot_size) {
    g_slot_a_buf.assign(slot_size, 0xFF);
    g_slot_b_buf.assign(slot_size, 0xFF);
    memcpy(g_slot_a_buf.data(), base_data, base_len);
    memset(g_meta, 0x00, TMD_PORT_META_SIZE);

    g_slot_a    = g_slot_a_buf.data();
    g_slot_b    = g_slot_b_buf.data();
    g_slot_size = slot_size;
    g_active    = 0;

    g_layout.slotA.addr = 0;
    g_layout.slotA.size = (uint32_t)slot_size;
    g_layout.slotB.addr = (uint32_t)slot_size;
    g_layout.slotB.size = (uint32_t)slot_size;
    g_layout.meta_addr  = (uint32_t)(slot_size * 2);
    g_layout.meta_size  = (uint32_t)TMD_PORT_META_SIZE;
}

/*
 * After a successful patch apply, returns a pointer to the updated model
 * data and sets *out_len to the model size (trailing 0xFF trimmed).
 */
static const uint8_t *tmd_mem_result(size_t *out_len) {
    const uint8_t *data = (g_active == 0) ? g_slot_a_buf.data() : g_slot_b_buf.data();
    size_t len = g_slot_size;
    while (len > 0 && data[len - 1] == 0xFF) --len;
    *out_len = len;
    return data;
}

/*
 * Release backing storage after the result has been consumed.
 */
static void tmd_mem_cleanup() {
    g_slot_a = g_slot_b = nullptr;
    g_slot_a_buf.clear();
    g_slot_b_buf.clear();
}

// =============================================================================
// Flash primitives
// =============================================================================

static uint8_t *g_addr_ptr(uint32_t addr, uint32_t len) {
    uint32_t sa_end = (uint32_t)g_slot_size;
    uint32_t sb_end = (uint32_t)(g_slot_size * 2);
    uint32_t me_end = (uint32_t)(g_slot_size * 2 + TMD_PORT_META_SIZE);
    if (addr          < sa_end && addr + len <= sa_end) return g_slot_a + addr;
    if (addr >= sa_end && addr + len <= sb_end)         return g_slot_b + (addr - sa_end);
    if (addr >= sb_end && addr + len <= me_end)         return g_meta   + (addr - sb_end);
    return nullptr;
}

static bool g_flash_erase(uint32_t addr, uint32_t len) {
    uint8_t *p = g_addr_ptr(addr, len);
    if (!p) return false;
    memset(p, 0xFF, len);
    return true;
}
static bool g_flash_write(uint32_t addr, const void *src, uint32_t len) {
    uint8_t *p = g_addr_ptr(addr, len);
    if (!p || !src) return false;
    memcpy(p, src, len);
    return true;
}
static bool g_flash_read(uint32_t addr, void *dst, uint32_t len) {
    const uint8_t *p = g_addr_ptr(addr, len);
    if (!p || !dst) return false;
    memcpy(dst, p, len);
    return true;
}

// =============================================================================
// Integrity
// =============================================================================

#if TMD_FEAT_CRC32
static uint32_t g_crc32(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
#endif

// =============================================================================
// Slot management
// =============================================================================

static uint8_t g_get_active(void)         { return g_active; }
static bool    g_set_active(uint8_t idx)  { g_active = idx; return true; }

// =============================================================================
// Journal
// =============================================================================

#if TMD_FEAT_JOURNAL
static bool g_journal_read(tmd_journal_t *out) {
    if (!out) return false;
    memcpy(out, g_meta, sizeof(*out));
    return true;
}
static bool g_journal_write(const tmd_journal_t *in) {
    if (!in) return false;
    memcpy(g_meta, in, sizeof(*in));
    return true;
}
static bool g_journal_clear(void) {
    memset(g_meta, 0x00, sizeof(tmd_journal_t));
    return true;
}
#endif

// =============================================================================
// Logging
// =============================================================================

#if TMD_FEAT_LOG
static void g_tmd_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[TMD] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}
#endif

// =============================================================================
// Port struct + C linkage exports
// =============================================================================

static const tmd_ports_t g_ports = {
    .flash_erase     = g_flash_erase,
    .flash_write     = g_flash_write,
    .flash_read      = g_flash_read,
#if TMD_FEAT_CRC32
    .crc32           = g_crc32,
#endif
    .get_active_slot = g_get_active,
    .set_active_slot = g_set_active,
#if TMD_FEAT_JOURNAL
    .journal_read    = g_journal_read,
    .journal_write   = g_journal_write,
    .journal_clear   = g_journal_clear,
#endif
#if TMD_FEAT_LOG
    .log             = g_tmd_log,
#endif
};

extern "C" const tmd_ports_t  *tmd_ports(void)  { return &g_ports;  }
extern "C" const tmd_layout_t *tmd_layout(void) { return &g_layout; }

#endif // TMD_PORT_MEMORY_H_
