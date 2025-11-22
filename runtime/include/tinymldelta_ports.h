#ifndef TINYMLDELTA_PORTS_H_
#define TINYMLDELTA_PORTS_H_
/**
 * @file tinymldelta_ports.h
 * @brief TinyMLDelta — Platform abstraction (flash, digests, slots, journal, log).
 * @author Felix Galindo
 * @license Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tinymldelta_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t addr;
  uint32_t size;
} tmd_slot_t;

typedef struct {
  tmd_slot_t slotA;
  tmd_slot_t slotB;
  uint32_t   meta_addr;
  uint32_t   meta_size;
} tmd_layout_t;

typedef struct {
  uint32_t magic;
  uint32_t patch_id;
  uint32_t next_chunk_idx;
  uint8_t  target_slot;
} tmd_journal_t;

typedef struct {
  bool (*flash_erase)(uint32_t addr, uint32_t len);
  bool (*flash_write)(uint32_t addr, const void* src, uint32_t len);
  bool (*flash_read )(uint32_t addr, void* dst,  uint32_t len);

#if TMD_FEAT_CRC32
  uint32_t (*crc32)(const void* buf, size_t len);
#endif
#if TMD_FEAT_SHA256
  void (*sha256_init)(void* ctx);
  void (*sha256_update)(void* ctx, const void* p, size_t n);
  void (*sha256_final)(void* ctx, uint8_t out[32]);
#endif
#if TMD_FEAT_AES_CMAC
  bool (*cmac_verify)(const uint8_t key16[16], const uint8_t* msg, size_t n,
                      const uint8_t tag16[16]);
#endif

  uint8_t (*get_active_slot)(void);
  bool    (*set_active_slot)(uint8_t idx);
#if TMD_FEAT_JOURNAL
  bool (*journal_read )(tmd_journal_t* out);
  bool (*journal_write)(const tmd_journal_t* in);
  bool (*journal_clear)(void);
#endif

#if TMD_FEAT_LOG
  void (*log)(const char* fmt, ...);
#endif
} tmd_ports_t;

const tmd_ports_t* tmd_ports(void);
const tmd_layout_t* tmd_layout(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYMLDELTA_PORTS_H_ */
