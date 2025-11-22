#ifndef TINYMLDELTA_INTERNAL_H_
#define TINYMLDELTA_INTERNAL_H_
/**
 * @file tinymldelta_internal.h
 * @brief TinyMLDelta — Wire format with metadata TLVs and chunk records.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
  uint8_t  v;
  uint8_t  algo;
  uint16_t chunks_n;
  uint32_t base_len;
  uint32_t target_len;
  uint8_t  base_chk[32];
  uint8_t  target_chk[32];
  uint16_t meta_len;
  uint16_t flags;
} tmd_hdr_t;

typedef struct __attribute__((packed)) {
  uint32_t off;
  uint16_t len;
  uint8_t  enc;
  uint8_t  has_crc;
} tmd_chunk_hdr_t;

typedef struct __attribute__((packed)) {
  uint8_t tag;
  uint8_t len;
} tmd_meta_tlv_t;

enum {
  TMD_META_REQ_ARENA_BYTES = 0x01,
  TMD_META_TFLM_ABI        = 0x02,
  TMD_META_OPSET_HASH      = 0x03,
  TMD_META_IO_HASH         = 0x04,
  TMD_META_VENDOR_BEGIN    = 0x80,
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYMLDELTA_INTERNAL_H_ */
