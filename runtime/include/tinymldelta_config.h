#ifndef TINYMLDELTA_CONFIG_H_
#define TINYMLDELTA_CONFIG_H_
/**
 * @file tinymldelta_config.h
 * @brief TinyMLDelta — Global build configuration and feature flags.
 *
 * This file centralizes *all* tunable build-time parameters for the
 * TinyMLDelta core:
 *
 *   • Integrity algorithms (CRC32, SHA-256, CMAC)
 *   • Compression options (RLE, optional LZ4Tiny)
 *   • Flash geometry (scratch buffer, alignments, sector size)
 *   • Journal/logging enable flags
 *   • Firmware-side guardrail limits (arena size, ABI version, op-set hash)
 *
 * MCU ports may override any value below at compile time (e.g., via
 * -D flags in the board’s build system).
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <stdint.h>

/* --------------------------------------------------------------------------
 *  Integrity Algorithms (pick exactly ONE)
 * --------------------------------------------------------------------------
 *
 * The patch header encodes a single integrity algorithm (algo field).
 * TinyMLDelta’s core enforces the chosen method and rejects mismatched patches.
 *
 * Options:
 *   TMD_NO_CHECK      — no integrity protection (testing only)
 *   TMD_USE_CRC32     — 32-bit integrity for chunks + header digests
 *   TMD_USE_CMAC_CRC  — AES-CMAC + CRC32 combo (future secure mode)
 *   TMD_USE_SHA256    — SHA-256 digest for stronger validation
 *
 * Exactly *one* of these must be enabled.
 */
#ifndef TMD_NO_CHECK
#define TMD_NO_CHECK      0
#endif
#ifndef TMD_USE_CRC32
#define TMD_USE_CRC32     1     /* Default: CRC32 is lightweight + MCU-friendly */
#endif
#ifndef TMD_USE_CMAC_CRC
#define TMD_USE_CMAC_CRC  0
#endif
#ifndef TMD_USE_SHA256
#define TMD_USE_SHA256    0
#endif

#if (TMD_NO_CHECK + TMD_USE_CRC32 + TMD_USE_CMAC_CRC + TMD_USE_SHA256) != 1
#error "Pick exactly ONE of: TMD_NO_CHECK, TMD_USE_CRC32, TMD_USE_CMAC_CRC, TMD_USE_SHA256"
#endif

/* Optional COSE signatures (future OTA authenticity layer). */
#ifndef TMD_USE_COSE_SIG
#define TMD_USE_COSE_SIG  0
#endif

/* --------------------------------------------------------------------------
 *  Compression Options
 * --------------------------------------------------------------------------
 *
 * RLE: Simple run-length encoding for repetitive deltas.
 * LZ4Tiny: Placeholder for future compressed chunk formats.
 */
#ifndef TMD_FEAT_RLE
#define TMD_FEAT_RLE      1
#endif
#ifndef TMD_FEAT_LZ4TINY
#define TMD_FEAT_LZ4TINY  0
#endif

/* --------------------------------------------------------------------------
 *  Flash & Buffer Geometry
 * --------------------------------------------------------------------------
 *
 * TMD_SCRATCH_SZ   — RAM scratch buffer for decoding chunks (RLE/LZ4Tiny).
 * TMD_ALIGN_WRITE  — Ensure writes align to flash driver requirements.
 * TMD_SECTOR_SZ    — Typical MCU flash erase sector size (4 KiB default).
 */
#ifndef TMD_SCRATCH_SZ
#define TMD_SCRATCH_SZ    1024
#endif
#ifndef TMD_ALIGN_WRITE
#define TMD_ALIGN_WRITE   4
#endif
#ifndef TMD_SECTOR_SZ
#define TMD_SECTOR_SZ     4096     /* Common value for most MCUs */
#endif

/* --------------------------------------------------------------------------
 *  Journal & Diagnostics
 * --------------------------------------------------------------------------
 *
 * Journaling enables crash-safe model updates. The journal tracks:
 *   • which patch is being applied
 *   • the next chunk index
 *   • which slot is the target
 *
 * Logging is optional; useful for debugging or POSIX simulations.
 */
#ifndef TMD_FEAT_JOURNAL
#define TMD_FEAT_JOURNAL  1
#endif
#ifndef TMD_FEAT_LOG
#define TMD_FEAT_LOG      1
#endif

/* --------------------------------------------------------------------------
 *  Derived features (do not modify manually)
 * --------------------------------------------------------------------------
 *
 * These map the chosen integrity algorithm to the actual features used by
 * tinymldelta_core.c.
 */
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
#error "Enable either CMAC OR SHA256 (not both)."
#endif

/* --------------------------------------------------------------------------
 *  Firmware Guardrail Settings
 * --------------------------------------------------------------------------
 *
 * These constants represent *firmware capabilities* baked into the MCU.
 * Patches will be rejected if:
 *
 *   • Required tensor arena > TMD_FIRMWARE_ARENA_BYTES
 *   • Required ABI > TMD_FIRMWARE_TFLM_ABI
 *   • Opset hash mismatch (if enabled)
 *   • IO hash mismatch (if enabled)
 *
 * These protect devices from incompatible or dangerous model upgrades.
 */

#ifndef TMD_FIRMWARE_ARENA_BYTES
#define TMD_FIRMWARE_ARENA_BYTES  (64 * 1024)   /* Example: 64 KiB arena */
#endif

#ifndef TMD_FIRMWARE_TFLM_ABI
#define TMD_FIRMWARE_TFLM_ABI      1            /* TFLM schema/ABI major */
#endif

#ifndef TMD_FIRMWARE_OPSET_HASH
#define TMD_FIRMWARE_OPSET_HASH    0u           /* 0 disables opset guardrail */
#endif

#ifndef TMD_ENFORCE_IO_HASH
#define TMD_ENFORCE_IO_HASH        0            /* Enable if you need shape/type strictness */
#endif

#ifndef TMD_FIRMWARE_IO_HASH
#define TMD_FIRMWARE_IO_HASH       0u           /* 0 = disabled */
#endif

#endif /* TINYMLDELTA_CONFIG_H_ */
