#ifndef TINYMLDELTA_CONFIG_H_
#define TINYMLDELTA_CONFIG_H_
/**
 * @file tinymldelta_config.h
 * @brief TinyMLDelta — Global build configuration and feature flags.
 * @author Felix Galindo
 * @license Apache-2.0
 */

#include <stdint.h>

/* Integrity (pick exactly one) */
#ifndef TMD_NO_CHECK
#define TMD_NO_CHECK      0
#endif
#ifndef TMD_USE_CRC32
#define TMD_USE_CRC32     1
#endif
#ifndef TMD_USE_CMAC_CRC
#define TMD_USE_CMAC_CRC  0
#endif
#ifndef TMD_USE_SHA256
#define TMD_USE_SHA256    0
#endif

#if (TMD_NO_CHECK + TMD_USE_CRC32 + TMD_USE_CMAC_CRC + TMD_USE_SHA256) != 1
#error "Pick exactly one of: TMD_NO_CHECK, TMD_USE_CRC32, TMD_USE_CMAC_CRC, TMD_USE_SHA256"
#endif

#ifndef TMD_USE_COSE_SIG
#define TMD_USE_COSE_SIG  0
#endif

/* Compression */
#ifndef TMD_FEAT_RLE
#define TMD_FEAT_RLE      1
#endif
#ifndef TMD_FEAT_LZ4TINY
#define TMD_FEAT_LZ4TINY  0
#endif

/* Flash & buffer geometry */
#ifndef TMD_SCRATCH_SZ
#define TMD_SCRATCH_SZ    1024
#endif
#ifndef TMD_ALIGN_WRITE
#define TMD_ALIGN_WRITE   4
#endif
#ifndef TMD_SECTOR_SZ
#define TMD_SECTOR_SZ     4096
#endif

/* Journal & diagnostics */
#ifndef TMD_FEAT_JOURNAL
#define TMD_FEAT_JOURNAL  1
#endif
#ifndef TMD_FEAT_LOG
#define TMD_FEAT_LOG      1
#endif

/* Derived features */
#if TMD_USE_CRC32 || TMD_USE_CMAC_CRC
  #ifndef TMD_FEAT_CRC32
  #define TMD_FEAT_CRC32  1
  #endif
#else
  #define TMD_FEAT_CRC32  0
#endif

#if TMD_USE_CMAC_CRC
  #ifndef TMD_FEAT_AES_CMAC
  #define TMD_FEAT_AES_CMAC 1
  #endif
#else
  #define TMD_FEAT_AES_CMAC 0
#endif

#if TMD_USE_SHA256
  #ifndef TMD_FEAT_SHA256
  #define TMD_FEAT_SHA256 1
  #endif
#else
  #define TMD_FEAT_SHA256 0
#endif

#if TMD_FEAT_AES_CMAC && TMD_FEAT_SHA256
#error "Enable either CMAC or SHA256, not both."
#endif

/* Firmware capability guardrails */
#ifndef TMD_FIRMWARE_ARENA_BYTES
#define TMD_FIRMWARE_ARENA_BYTES  (64 * 1024)
#endif

#ifndef TMD_FIRMWARE_TFLM_ABI
#define TMD_FIRMWARE_TFLM_ABI      1
#endif

#ifndef TMD_FIRMWARE_OPSET_HASH
#define TMD_FIRMWARE_OPSET_HASH    0u
#endif

#ifndef TMD_ENFORCE_IO_HASH
#define TMD_ENFORCE_IO_HASH        0
#endif
#ifndef TMD_FIRMWARE_IO_HASH
#define TMD_FIRMWARE_IO_HASH       0u
#endif

#endif /* TINYMLDELTA_CONFIG_H_ */
