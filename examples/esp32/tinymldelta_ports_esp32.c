/**
 * @file tinymldelta_ports_esp32.c
 * @brief ESP32 (ESP-IDF) platform port for the TinyMLDelta core.
 *
 * Implements the tmd_ports_t / tmd_layout_t HAL against ESP-IDF:
 *   - flash via the partition API (esp_partition_*) on a custom data partition
 *   - A/B slots + journal as offsets WITHIN that partition
 *   - active-slot index persisted in NVS
 *   - CRC-32 in software (zlib-compatible, matches PatchGen's digests)
 *
 * Reference port: written against the documented ESP-IDF API and structured like
 * the POSIX port. It must be validated on hardware (esp_partition write/erase
 * alignment, NVS init) before production use.
 *
 * SETUP (see README.md):
 *   1. Add a custom data partition named "tmd_models" sized >= 2*SLOT + 4 KiB
 *      (see partitions.csv).
 *   2. Slots and journal must be 4 KiB-sector aligned (they are with the
 *      defaults below).
 *   3. Call tmd_esp32_init() once at startup, then tmd_apply_patch_from_memory().
 *
 * License: Apache-2.0
 */
#include <string.h>

#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "tinymldelta_ports.h"
#include "tinymldelta_internal.h"   /* tmd_journal_t, magic */

#define TAG "tinymldelta"

/* ---- Geometry (4 KiB-aligned). Override with -D at build time. ----------- */
#ifndef TMD_ESP32_SLOT_BYTES
#define TMD_ESP32_SLOT_BYTES   (256u * 1024u)   /* per A/B slot */
#endif
#ifndef TMD_ESP32_META_BYTES
#define TMD_ESP32_META_BYTES   (4u * 1024u)     /* journal: one sector */
#endif
#define TMD_ESP32_SECTOR       (4u * 1024u)

static const esp_partition_t* g_part = NULL;     /* "tmd_models" partition */

static tmd_layout_t g_layout = {
    .slotA     = { .addr = 0u,                       .size = TMD_ESP32_SLOT_BYTES },
    .slotB     = { .addr = TMD_ESP32_SLOT_BYTES,     .size = TMD_ESP32_SLOT_BYTES },
    .meta_addr = 2u * TMD_ESP32_SLOT_BYTES,
    .meta_size = TMD_ESP32_META_BYTES,
};

/* ---- Flash primitives (partition-relative offsets) ----------------------- */

static bool esp32_flash_read(uint32_t addr, void* dst, uint32_t len) {
  return g_part && esp_partition_read(g_part, addr, dst, len) == ESP_OK;
}

static bool esp32_flash_write(uint32_t addr, const void* src, uint32_t len) {
  return g_part && esp_partition_write(g_part, addr, src, len) == ESP_OK;
}

/* esp_partition_erase_range requires 4 KiB-aligned offset and length. The core
 * erases whole (sector-aligned) slots, so round up to be safe. */
static bool esp32_flash_erase(uint32_t addr, uint32_t len) {
  uint32_t end = (addr + len + TMD_ESP32_SECTOR - 1u) & ~(TMD_ESP32_SECTOR - 1u);
  uint32_t start = addr & ~(TMD_ESP32_SECTOR - 1u);
  return g_part && esp_partition_erase_range(g_part, start, end - start) == ESP_OK;
}

/* ---- CRC-32 (software, zlib-compatible — matches PatchGen) ---------------- */

static uint32_t crc32_sw(const void* buf, size_t len) {
  const uint8_t* p = (const uint8_t*)buf;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

/* ---- Active-slot index in NVS -------------------------------------------- */

static uint8_t esp32_get_active_slot(void) {
  nvs_handle_t h;
  uint8_t idx = 0;
  if (nvs_open("tinymldelta", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "active", &idx);       /* default 0 if unset */
    nvs_close(h);
  }
  return idx ? 1 : 0;
}

static bool esp32_set_active_slot(uint8_t idx) {
  nvs_handle_t h;
  if (nvs_open("tinymldelta", NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_u8(h, "active", idx ? 1 : 0) == ESP_OK &&
            nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

/* ---- Journal (in the meta region) ---------------------------------------- */

static bool esp32_journal_read(tmd_journal_t* out) {
  return esp32_flash_read(g_layout.meta_addr, out, sizeof(*out));
}

static bool esp32_journal_write(const tmd_journal_t* in) {
  if (!esp32_flash_erase(g_layout.meta_addr, sizeof(*in))) return false;
  return esp32_flash_write(g_layout.meta_addr, in, sizeof(*in));
}

static bool esp32_journal_clear(void) {
  tmd_journal_t zero;
  memset(&zero, 0, sizeof(zero));
  return esp32_journal_write(&zero);
}

/* ---- Logging ------------------------------------------------------------- */

static void esp32_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  esp_log_writev(ESP_LOG_INFO, TAG, fmt, ap);
  va_end(ap);
}

/* ---- Ports table --------------------------------------------------------- */

static tmd_ports_t g_ports = {
    .flash_erase     = esp32_flash_erase,
    .flash_write     = esp32_flash_write,
    .flash_read      = esp32_flash_read,
    .crc32           = crc32_sw,
    .get_active_slot = esp32_get_active_slot,
    .set_active_slot = esp32_set_active_slot,
    .journal_read    = esp32_journal_read,
    .journal_write   = esp32_journal_write,
    .journal_clear   = esp32_journal_clear,
    .log             = esp32_log,
};

const tmd_ports_t*  tmd_ports(void)  { return &g_ports;  }
const tmd_layout_t* tmd_layout(void) { return &g_layout; }

/**
 * @brief One-time init: NVS + locate the "tmd_models" partition.
 * @return true on success.
 */
bool tmd_esp32_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) return false;

  g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, "tmd_models");
  if (!g_part) {
    ESP_LOGE(TAG, "partition 'tmd_models' not found");
    return false;
  }
  if (g_part->size < 2u * TMD_ESP32_SLOT_BYTES + TMD_ESP32_META_BYTES) {
    ESP_LOGE(TAG, "partition too small: %u < %u", (unsigned)g_part->size,
             (unsigned)(2u * TMD_ESP32_SLOT_BYTES + TMD_ESP32_META_BYTES));
    return false;
  }
  return true;
}
