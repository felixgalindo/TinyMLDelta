/**
 * @file tinymldelta_ports_posix.c
 * @brief TinyMLDelta ports implementation on top of a flash.bin file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tinymldelta_ports.h"
#include "flash_layout.h"

static const char* g_flash_path = NULL;
static FILE* g_flash_fp = NULL;
static const char* g_active_slot_path = NULL;

static int ensure_flash_open(void) {
  if (!g_flash_path) return -1;
  if (!g_flash_fp) {
    g_flash_fp = fopen(g_flash_path, "r+b");
    if (!g_flash_fp) {
      g_flash_fp = fopen(g_flash_path, "w+b");
      if (!g_flash_fp) return -1;
    }
  }
  return 0;
}

static bool posix_flash_erase(uint32_t addr, uint32_t len) {
  if (ensure_flash_open() != 0) return false;
  if (fseek(g_flash_fp, (long)addr, SEEK_SET) != 0) return false;
  uint8_t buf[256];
  memset(buf, 0xFF, sizeof(buf));
  while (len > 0) {
    uint32_t chunk = len > sizeof(buf) ? sizeof(buf) : len;
    if (fwrite(buf, 1, chunk, g_flash_fp) != chunk) return false;
    len -= chunk;
  }
  fflush(g_flash_fp);
  return true;
}

static bool posix_flash_write(uint32_t addr, const void* src, uint32_t len) {
  if (ensure_flash_open() != 0) return false;
  if (fseek(g_flash_fp, (long)addr, SEEK_SET) != 0) return false;
  if (fwrite(src, 1, len, g_flash_fp) != len) return false;
  fflush(g_flash_fp);
  return true;
}

static bool posix_flash_read(uint32_t addr, void* dst, uint32_t len) {
  if (ensure_flash_open() != 0) return false;
  if (fseek(g_flash_fp, (long)addr, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, g_flash_fp) != len) return false;
  return true;
}

#if TMD_FEAT_CRC32
static uint32_t crc32_sw(const void* buf, size_t len) {
  /* Very small SW CRC32 for the example (not optimized). */
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

static uint8_t posix_get_active_slot(void) {
  if (!g_active_slot_path) return 0;
  FILE* f = fopen(g_active_slot_path, "rb");
  if (!f) return 0;
  int c = fgetc(f);
  fclose(f);
  if (c == '1') return 1;
  return 0;
}

static bool posix_set_active_slot(uint8_t idx) {
  if (!g_active_slot_path) return false;
  FILE* f = fopen(g_active_slot_path, "wb");
  if (!f) return false;
  fputc(idx ? '1' : '0', f);
  fclose(f);
  return true;
}

#if TMD_FEAT_JOURNAL
static bool posix_journal_read(tmd_journal_t* out) {
  if (!out) return false;
  if (ensure_flash_open() != 0) return false;
  if (fseek(g_flash_fp, (long)g_layout.meta_addr, SEEK_SET) != 0) return false;
  if (fread(out, 1, sizeof(*out), g_flash_fp) != sizeof(*out)) {
    memset(out, 0, sizeof(*out));
  }
  return true;
}

static bool posix_journal_write(const tmd_journal_t* in) {
  if (!in) return false;
  if (ensure_flash_open() != 0) return false;
  if (fseek(g_flash_fp, (long)g_layout.meta_addr, SEEK_SET) != 0) return false;
  if (fwrite(in, 1, sizeof(*in), g_flash_fp) != sizeof(*in)) return false;
  fflush(g_flash_fp);
  return true;
}

static bool posix_journal_clear(void) {
  tmd_journal_t zero;
  memset(&zero, 0, sizeof(zero));
  return posix_journal_write(&zero);
}
#endif

#if TMD_FEAT_LOG
#include <stdarg.h>
static void posix_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
#endif

static tmd_ports_t g_ports = {
  .flash_erase = posix_flash_erase,
  .flash_write = posix_flash_write,
  .flash_read  = posix_flash_read,
#if TMD_FEAT_CRC32
  .crc32 = crc32_sw,
#endif
#if TMD_FEAT_SHA256
  .sha256_init = NULL,
  .sha256_update = NULL,
  .sha256_final = NULL,
#endif
#if TMD_FEAT_AES_CMAC
  .cmac_verify = NULL,
#endif
  .get_active_slot = posix_get_active_slot,
  .set_active_slot = posix_set_active_slot,
#if TMD_FEAT_JOURNAL
  .journal_read  = posix_journal_read,
  .journal_write = posix_journal_write,
  .journal_clear = posix_journal_clear,
#endif
#if TMD_FEAT_LOG
  .log = posix_log,
#endif
};

const tmd_ports_t* tmd_ports(void) {
  return &g_ports;
}

const tmd_layout_t* tmd_layout(void) {
  return &g_layout;
}

/* Helpers to configure file paths from the demo app. */
void tmd_posix_set_flash_path(const char* path) {
  g_flash_path = path;
}
void tmd_posix_set_active_slot_path(const char* path) {
  g_active_slot_path = path;
}
