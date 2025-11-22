/**
 * @file tinymldelta_ports_posix.c
 * @brief TinyMLDelta ports implementation on top of a flash.bin file.
 *
 * This file implements the TinyMLDelta platform ports API using a simple
 * POSIX file-backed "flash" image (flash.bin) and a small text file
 * (active_slot.txt) to track which slot is currently active.
 *
 * Author: Felix Galindo
 * License: Apache-2.0
 *
 * -----------------------------------------------------------------------------
 * OVERVIEW
 * -----------------------------------------------------------------------------
 *
 *  - flash.bin
 *      Represents NOR flash as a flat binary file.
 *      The layout is described by flash_layout.h (g_layout).
 *
 *  - active_slot.txt
 *      A one-byte text file storing '0' or '1' to indicate which slot is
 *      currently active. The TinyMLDelta runtime uses this to determine
 *      the "source" slot (A or B) and which slot to patch into.
 *
 *  - Journal (optional)
 *      When TMD_FEAT_JOURNAL is enabled, a small tmd_journal_t structure is
 *      stored at g_layout.meta_addr within flash.bin. This allows recovery
 *      of partially-applied patches after a reset or power loss.
 *
 * This POSIX port is purely for demos and tests; real MCU ports should
 * enforce flash geometry, erase block sizes, alignment rules, and wear
 * leveling as required by the underlying hardware.
 *
 * -----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "tinymldelta_ports.h"
#include "flash_layout.h"

/* -------------------------------------------------------------------------- */
/* Global state for POSIX-backed flash + active-slot tracking                 */
/* -------------------------------------------------------------------------- */

/* Path to the flash image (flash.bin), configured by tmd_posix_set_flash_path. */
static const char* g_flash_path = NULL;
/* File handle used for all flash read/write/erase operations. */
static FILE* g_flash_fp = NULL;
/* Path to the active-slot marker file (e.g., active_slot.txt). */
static const char* g_active_slot_path = NULL;

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ensure g_flash_fp is open and ready for I/O.
 *
 * If flash.bin does not exist yet, it is created. For this POSIX demo we rely
 * on examples/posix/make_flash.py to size and initialize the file, so we do
 * not attempt to grow it or enforce size here.
 *
 * @return 0 on success, -1 on failure.
 */
static int ensure_flash_open(void) {
  if (!g_flash_path) {
    return -1;
  }
  if (!g_flash_fp) {
    /* Try to open existing flash.bin for read/write. */
    g_flash_fp = fopen(g_flash_path, "r+b");
    if (!g_flash_fp) {
      /* If it does not exist, create a new one. Caller (demo) is responsible
       * for making sure it has the right size and contents. */
      g_flash_fp = fopen(g_flash_path, "w+b");
      if (!g_flash_fp) {
        return -1;
      }
    }
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Flash operations                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Erase a region of flash by writing 0xFF bytes.
 *
 * This is a simple model of NOR flash erase behavior. It does not enforce
 * sector boundaries or erase block size; it simply overwrites the requested
 * range with 0xFF.
 */
static bool posix_flash_erase(uint32_t addr, uint32_t len) {
  if (ensure_flash_open() != 0) {
    return false;
  }
  if (fseek(g_flash_fp, (long)addr, SEEK_SET) != 0) {
    return false;
  }

  uint8_t buf[256];
  memset(buf, 0xFF, sizeof(buf));

  while (len > 0) {
    uint32_t chunk = (len > (uint32_t)sizeof(buf)) ? (uint32_t)sizeof(buf) : len;
    if (fwrite(buf, 1, chunk, g_flash_fp) != chunk) {
      return false;
    }
    len -= chunk;
  }
  fflush(g_flash_fp);
  return true;
}

/**
 * @brief Write a range of bytes into flash.bin.
 *
 * No alignment or wear-leveling is modeled here; this is a straight file
 * write used for the POSIX TinyMLDelta demo.
 */
static bool posix_flash_write(uint32_t addr, const void* src, uint32_t len) {
  if (ensure_flash_open() != 0) {
    return false;
  }
  if (fseek(g_flash_fp, (long)addr, SEEK_SET) != 0) {
    return false;
  }
  if (fwrite(src, 1, len, g_flash_fp) != len) {
    return false;
  }
  fflush(g_flash_fp);
  return true;
}

/**
 * @brief Read a range of bytes from flash.bin.
 */
static bool posix_flash_read(uint32_t addr, void* dst, uint32_t len) {
  if (ensure_flash_open() != 0) {
    return false;
  }
  if (fseek(g_flash_fp, (long)addr, SEEK_SET) != 0) {
    return false;
  }
  if (fread(dst, 1, len, g_flash_fp) != len) {
    return false;
  }
  return true;
}

/* -------------------------------------------------------------------------- */
/* CRC32 (software)                                                           */
/* -------------------------------------------------------------------------- */

#if TMD_FEAT_CRC32
/**
 * @brief Very small software CRC32 implementation (not optimized).
 *
 * This is sufficient for the POSIX demo and keeps the runtime fully
 * self-contained. Real firmware may wire this to a hardware CRC engine
 * or a more optimized library implementation.
 */
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
#endif /* TMD_FEAT_CRC32 */

/* -------------------------------------------------------------------------- */
/* Active slot tracking (active_slot.txt)                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Return which slot is currently active (0 or 1).
 *
 * The active slot is stored as a single character ('0' or '1') in
 * g_active_slot_path. If the file does not exist or cannot be read,
 * slot 0 is assumed to be active.
 */
static uint8_t posix_get_active_slot(void) {
  if (!g_active_slot_path) {
    return 0;
  }
  FILE* f = fopen(g_active_slot_path, "rb");
  if (!f) {
    return 0;
  }
  int c = fgetc(f);
  fclose(f);
  if (c == '1') {
    return 1;
  }
  return 0;
}

/**
 * @brief Persist the active slot index (0 or 1) into active_slot.txt.
 */
static bool posix_set_active_slot(uint8_t idx) {
  if (!g_active_slot_path) {
    return false;
  }
  FILE* f = fopen(g_active_slot_path, "wb");
  if (!f) {
    return false;
  }
  fputc(idx ? '1' : '0', f);
  fclose(f);
  return true;
}

/* -------------------------------------------------------------------------- */
/* Journal support (optional)                                                 */
/* -------------------------------------------------------------------------- */

#if TMD_FEAT_JOURNAL
/**
 * @brief Read the TinyMLDelta journal structure from flash.
 *
 * The journal location is defined by g_layout.meta_addr. On a short read,
 * the output structure is zeroed and true is still returned so the caller
 * can treat it as "no journal present".
 */
static bool posix_journal_read(tmd_journal_t* out) {
  if (!out) {
    return false;
  }
  if (ensure_flash_open() != 0) {
    return false;
  }
  if (fseek(g_flash_fp, (long)g_layout.meta_addr, SEEK_SET) != 0) {
    return false;
  }
  if (fread(out, 1, sizeof(*out), g_flash_fp) != sizeof(*out)) {
    memset(out, 0, sizeof(*out));
  }
  return true;
}

/**
 * @brief Write the TinyMLDelta journal structure to flash.
 */
static bool posix_journal_write(const tmd_journal_t* in) {
  if (!in) {
    return false;
  }
  if (ensure_flash_open() != 0) {
    return false;
  }
  if (fseek(g_flash_fp, (long)g_layout.meta_addr, SEEK_SET) != 0) {
    return false;
  }
  if (fwrite(in, 1, sizeof(*in), g_flash_fp) != sizeof(*in)) {
    return false;
  }
  fflush(g_flash_fp);
  return true;
}

/**
 * @brief Clear the journal by writing a zeroed tmd_journal_t.
 */
static bool posix_journal_clear(void) {
  tmd_journal_t zero;
  memset(&zero, 0, sizeof(zero));
  return posix_journal_write(&zero);
}
#endif /* TMD_FEAT_JOURNAL */

/* -------------------------------------------------------------------------- */
/* Logging (optional)                                                         */
/* -------------------------------------------------------------------------- */

#if TMD_FEAT_LOG
#include <stdarg.h>

/**
 * @brief Simple logger that prints to stderr.
 *
 * TinyMLDelta core calls this via TMD_LOG(...) when TMD_FEAT_LOG is enabled.
 */
static void posix_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
#endif /* TMD_FEAT_LOG */

/* -------------------------------------------------------------------------- */
/* TinyMLDelta ports + layout accessors                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Global ports instance used by TinyMLDelta core on POSIX.
 */
static tmd_ports_t g_ports = {
  .flash_erase = posix_flash_erase,
  .flash_write = posix_flash_write,
  .flash_read  = posix_flash_read,
#if TMD_FEAT_CRC32
  .crc32 = crc32_sw,
#endif
#if TMD_FEAT_SHA256
  .sha256_init   = NULL,
  .sha256_update = NULL,
  .sha256_final  = NULL,
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

/**
 * @brief Return the active ports implementation for this platform.
 */
const tmd_ports_t* tmd_ports(void) {
  return &g_ports;
}

/**
 * @brief Return the flash layout in use (slots, meta region).
 */
const tmd_layout_t* tmd_layout(void) {
  return &g_layout;
}

/* -------------------------------------------------------------------------- */
/* POSIX-specific helpers used by demo_apply.c                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Configure the path to the POSIX flash image (flash.bin).
 *
 * The demo app calls this once before invoking TinyMLDelta APIs.
 */
void tmd_posix_set_flash_path(const char* path) {
  g_flash_path = path;
}

/**
 * @brief Configure the path to the active-slot marker file.
 */
void tmd_posix_set_active_slot_path(const char* path) {
  g_active_slot_path = path;
}
