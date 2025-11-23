#ifndef TINYMLDELTA_PORTS_H_
#define TINYMLDELTA_PORTS_H_

/**
 * @file tinymldelta_ports.h
 * @brief TinyMLDelta — Platform abstraction layer.
 *
 * This header defines the interface that *all platforms must implement*
 * in order to support TinyMLDelta patch application on MCUs or simulated
 * environments (POSIX, Zephyr, Arduino, XBee, etc.).
 *
 * The port provides:
 *   - Flash read/erase/write primitives
 *   - Integrity functions (CRC32, SHA-256, CMAC) depending on config
 *   - A/B slot selection (active/inactive)
 *   - Crash-safe journal access
 *   - Optional logging
 *
 * This design keeps the TinyMLDelta core 100% platform-agnostic.
 *
 * Author: Felix Galindo
 * License: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tinymldelta_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Slot definition
 * --------------------------------------------------------------------------
 * Models live in fixed flash regions known as "slots."
 * TinyMLDelta always maintains:
 *    slotA = one full model
 *    slotB = another full model
 *
 * During patching:
 *    active   = currently used model
 *    inactive = copy of active, then patched into new version
 *
 * addr: byte offset into flash
 * size: length of slot in bytes (must match between A/B)
 */
typedef struct {
  uint32_t addr;   /**< Flash address (offset) of slot start */
  uint32_t size;   /**< Size of slot in bytes */
} tmd_slot_t;

/* --------------------------------------------------------------------------
 * Flash layout definition
 * --------------------------------------------------------------------------
 * Platforms define:
 *   - slotA: location + size
 *   - slotB: location + size
 *   - meta_addr/meta_size: region reserved for journaling or metadata pages
 *
 * The core uses these values to copy slots atomically and safely.
 */
typedef struct {
  tmd_slot_t slotA;     /**< Primary model slot */
  tmd_slot_t slotB;     /**< Secondary model slot */
  uint32_t   meta_addr; /**< Flash region used for journaling */
  uint32_t   meta_size; /**< Journal size in bytes */
} tmd_layout_t;

/* --------------------------------------------------------------------------
 * Journal structure
 * --------------------------------------------------------------------------
 * The journal stores progress during patching so updates are crash-resistant.
 *
 * Fields:
 *   magic          - Identifies journal validity ('TMDP')
 *   patch_id       - Optional: ID of patch being applied
 *   next_chunk_idx - Which diff chunk still needs to be applied
 *   target_slot    - Which slot is being written (0 or 1)
 *
 * After a reboot mid-update, the core resumes from next_chunk_idx.
 */
typedef struct {
  uint32_t magic;          /**< Must match TMD_JOURNAL_MAGIC */
  uint32_t patch_id;       /**< Optional patch identifier */
  uint32_t next_chunk_idx; /**< Next unapplied chunk index */
  uint8_t  target_slot;    /**< Destination slot (0=A, 1=B) */
} tmd_journal_t;

/* --------------------------------------------------------------------------
 * Platform port interface
 * --------------------------------------------------------------------------
 * Each MCU/platform must implement this function table.
 *
 * The TinyMLDelta core ONLY communicates through these pointers.
 *
 * All functions must obey flash constraints:
 *   - erase must align to sector boundaries (platform-specific)
 *   - write must respect write-granularity (platform-specific)
 *   - read must behave like raw flash reads
 */
typedef struct {
  /* -------------------- Flash primitives -------------------- */
  bool (*flash_erase)(uint32_t addr, uint32_t len);
      /**< Erase len bytes starting at addr. Fills region with 0xFF. */

  bool (*flash_write)(uint32_t addr, const void* src, uint32_t len);
      /**< Write len bytes at addr. Must support partial-page writes if MCU allows. */

  bool (*flash_read)(uint32_t addr, void* dst, uint32_t len);
      /**< Read len bytes from addr into dst. */

  /* -------------------- Integrity algorithms -------------------- */
#if TMD_FEAT_CRC32
  uint32_t (*crc32)(const void* buf, size_t len);
      /**< Optional: CRC32 checksum used for chunk-level integrity. */
#endif

#if TMD_FEAT_SHA256
  void (*sha256_init)(void* ctx);
  void (*sha256_update)(void* ctx, const void* p, size_t n);
  void (*sha256_final)(void* ctx, uint8_t out[32]);
      /**< Optional: SHA-256 support for signed patches. */
#endif

#if TMD_FEAT_AES_CMAC
  bool (*cmac_verify)(const uint8_t key16[16], const uint8_t* msg, size_t n,
                      const uint8_t tag16[16]);
      /**< Optional: AES-CMAC for secure model update signing. */
#endif

  /* -------------------- Slot switching -------------------- */
  uint8_t (*get_active_slot)(void);
      /**< Return current active model slot: 0 = A, 1 = B. */

  bool (*set_active_slot)(uint8_t idx);
      /**< Atomically commit slot A/B selection. Called AFTER a successful patch. */

  /* -------------------- Crash-safe journal -------------------- */
#if TMD_FEAT_JOURNAL
  bool (*journal_read)(tmd_journal_t* out);
      /**< Load journal state from flash (may be all zero if no journal). */

  bool (*journal_write)(const tmd_journal_t* in);
      /**< Store updated journal state during patch application. */

  bool (*journal_clear)(void);
      /**< Clear journal after a successful update. */
#endif

  /* -------------------- Optional logging -------------------- */
#if TMD_FEAT_LOG
  void (*log)(const char* fmt, ...);
      /**< Optional printf-like logger (stderr on POSIX). */
#endif

} tmd_ports_t;

/* --------------------------------------------------------------------------
 * Returns the platform’s port table.
 *
 * Must be implemented by the platform (POSIX, MCU, Zephyr, Arduino, etc.).
 * The TinyMLDelta core calls tmd_ports()->flash_*(), tmd_ports()->crc32(), etc.
 * -------------------------------------------------------------------------- */
const tmd_ports_t* tmd_ports(void);

/* --------------------------------------------------------------------------
 * Returns flash layout containing slot addresses and journal location.
 *
 * Defined per platform so the core knows where slotA, slotB, and meta regions live.
 * -------------------------------------------------------------------------- */
const tmd_layout_t* tmd_layout(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYMLDELTA_PORTS_H_ */
