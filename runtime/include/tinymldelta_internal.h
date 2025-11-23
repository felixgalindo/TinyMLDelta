#ifndef TINYMLDELTA_INTERNAL_H_
#define TINYMLDELTA_INTERNAL_H_
/**
 * @file tinymldelta_internal.h
 * @brief TinyMLDelta — Wire format (patch header, metadata TLVs, chunk records).
 *
 * This header defines the exact binary layout of a TinyMLDelta patch (.tmd):
 *
 *   • Patch header (`tmd_hdr_t`)
 *   • Metadata TLV block (`tmd_meta_tlv_t`)
 *   • Per-chunk descriptors (`tmd_chunk_hdr_t`)
 *
 * The TinyMLDelta core (`tinymldelta_core.c`) consumes this format
 * directly without modifying it. All fields are *little-endian* and
 * `packed` so that patches can be streamed or stored exactly as-is.
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 *  Patch Header
 * --------------------------------------------------------------------------
 *
 * Each patch begins with a fixed-size header. It is followed by:
 *   1) metadata TLV block (meta_len bytes)
 *   2) chunk records (chunks_n entries)
 *
 * The header also contains:
 *   • CRC32 / SHA-256 / CMAC digests of base + target model images
 *   • Full model lengths (for safety + consistency checks)
 *   • Format version (v=1 for now)
 */

typedef struct __attribute__((packed)) {
  uint8_t  v;          /**< Format version. Must be 1 for this implementation. */

  uint8_t  algo;       /**< Integrity algorithm:
                            0 = none
                            1 = CRC32
                            2 = SHA256
                            3 = CMAC+CRC   (reserved for future)
                        */

  uint16_t chunks_n;   /**< Number of diff chunks in this patch. */

  uint32_t base_len;   /**< Size of the base model in bytes, as observed by the
                            patch generator. Used to detect unexpected flashing. */

  uint32_t target_len; /**< Size of the target model in bytes. */

  uint8_t  base_chk[32];   /**< Digest of base model (CRC padded to 32 bytes,
                                or SHA-256, or CMAC+CRC). */

  uint8_t  target_chk[32]; /**< Digest of target model (same algorithm as above). */

  uint16_t meta_len;   /**< Total size in bytes of the metadata TLV block
                            immediately following this header. */

  uint16_t flags;      /**< Reserved for future extensions (semantic versioning,
                            signature flags, encryption policy, etc.). */
} tmd_hdr_t;


/* --------------------------------------------------------------------------
 *  Chunk Record Header
 * --------------------------------------------------------------------------
 *
 * Each diff chunk describes a contiguous overwrite to be applied to the
 * inactive model slot. The payload data follows immediately after the
 * chunk header (plus optional CRC32).
 *
 * Layout:
 *    [tmd_chunk_hdr_t]
 *    [optional CRC32 (4 bytes)]
 *    [payload bytes...]
 *
 * Payload encoding may be:
 *    • RAW (verbatim bytes)
 *    • RLE (simple run-length encoding)
 *
 * The core validates chunk bounds before writing to flash.
 */

typedef struct __attribute__((packed)) {
  uint32_t off;      /**< Byte offset inside the model image where payload
                          should be written (relative to slot base). */

  uint16_t len;      /**< Length of encoded payload in bytes (RAW or RLE). */

  uint8_t  enc;      /**< Encoding:
                          0 = RAW
                          1 = RLE
                          Future: LZ4Tiny, etc. */

  uint8_t  has_crc;  /**< If 1, a CRC32 appears immediately before payload. */
} tmd_chunk_hdr_t;


/* --------------------------------------------------------------------------
 *  Metadata TLV (Type-Length-Value)
 * --------------------------------------------------------------------------
 *
 * The metadata section encodes safety guardrails and optional fields.
 *
 * Each TLV:
 *    tag  — identifying what the metadata entry represents
 *    len  — size of the value payload that follows
 *    value[len]
 *
 * Vendor-specific TLVs start at tag >= 0x80.
 */

typedef struct __attribute__((packed)) {
  uint8_t tag;   /**< Metadata tag ID. */
  uint8_t len;   /**< Length of the following value. */
} tmd_meta_tlv_t;


/* --------------------------------------------------------------------------
 *  Standard Metadata Tags
 * --------------------------------------------------------------------------
 *
 * The TinyMLDelta core recognizes these tags for guardrail enforcement:
 *
 *   TMD_META_REQ_ARENA_BYTES — minimum required tensor arena size
 *   TMD_META_TFLM_ABI        — required ABI/schema version for TFLM kernels
 *   TMD_META_OPSET_HASH      — hash of builtin operator usage
 *   TMD_META_IO_HASH         — hash of the model’s tensor shapes + dtypes
 *
 * Tags >= TMD_META_VENDOR_BEGIN are vendor/user-defined and ignored by core
 * unless a port specifically interprets them.
 */

enum {
  TMD_META_REQ_ARENA_BYTES = 0x01, /**< u32 — required arena size in bytes. */
  TMD_META_TFLM_ABI        = 0x02, /**< u16 — required TFLM ABI/schema. */
  TMD_META_OPSET_HASH      = 0x03, /**< u32 — builtin operator set hash. */
  TMD_META_IO_HASH         = 0x04, /**< u32 — tensor I/O signature hash. */

  /** Vendor or platform-specific TLVs begin here. */
  TMD_META_VENDOR_BEGIN    = 0x80,
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYMLDELTA_INTERNAL_H_ */
