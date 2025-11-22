# TinyMLDelta

TinyMLDelta is an **incremental model-update system** for TinyML and embedded AI devices.

Instead of shipping an entire TensorFlow Lite Micro model (often **20–200+ KB**),
you ship a **small binary patch** that mutates the existing model in flash into a new one.

This reduces:

- OTA bandwidth
- Cellular / satellite data costs
- Flash wear
- Update latency
- Fleet fragmentation
- Bootloader complexity

TinyMLDelta performs **safe, atomic, guardrail-checked updates** on extremely low-resource MCUs.

## Supported Today

- TensorFlow Lite Micro
- POSIX/macOS simulated flash environment
- CRC32 integrity checking
- A/B slot updates
- Crash-safe journaling

## Planned
- Edge Impulse frontend
- SHA-256 and AES-CMAC signatures
- Model versioning
- Vendor metadata
- MCU integrations (Zephyr, Arduino UNO R4 WiFi, Tachyon)

## High-Level Design

```
          ┌────────────────────────┐
          │ Base Model (flash)     │
          └──────────┬─────────────┘
                     │ read
                     ▼
           ┌────────────────────────┐
           │ Patch Generator (PC)   │
           │  • diff engine         │
           │  • metadata TLVs       │
           │  • integrity checks    │
           └──────────┬─────────────┘
                     │ OTA send
                     ▼
           ┌────────────────────────┐
           │ TinyMLDelta Core (MCU) │
           │  • copy active→inactive│
           │  • apply diff chunks   │
           │  • verify CRC/digests  │
           │  • enforce guardrails  │
           └──────────┬─────────────┘
                     │ atomically flip
                     ▼
           ┌────────────────────────┐
           │ Target Model (flash)   │
           └────────────────────────┘
```

The MCU **never** receives a full model—only a small delta.

## Patch Generation (PC / CI)

```bash
python3 cli/tinymldelta_patchgen.py     base.tflite     target.tflite     out_patch.tmd     --auto-meta
```

This produces a `.tmd` patch containing:

- A header (`tmd_hdr_t`)
- **Metadata TLVs** (guardrails + compatibility info)
- One or more diff chunks  
- Optional CRC32 per-chunk

**Example output (from real run):**

```
Chunks: 1
Encoded bytes: 383
Base model: 67440 bytes
Target model: 67440 bytes
Patch size: 475 bytes
```

⚠ **Note:** Patch savings vary depending on model structure — this is one example, not a guarantee.

## Metadata TLVs (Needs Work – Roadmap Included)

TLVs = Type-Length-Value records embedded in patches.

Current core TLVs:

```
REQ_ARENA_BYTES   (u32)
TFLM_ABI          (u16)
OPSET_HASH        (u32)
IO_HASH           (u32)
```

These enable **compatibility guardrails**:

- Reject patches requiring a larger tensor arena  
- Reject patches requiring a newer TFLM ABI  
- Reject if built-in operators differ  
- Reject if tensor I/O schema (types + shapes) differs  

### TLV Roadmap (Work Needed)

The TLV subsystem will expand:

- Model version TLVs  
- Model family / architecture TLVs  
- Signature / certificate blocks  
- Vendor/private TLVs for custom constraints  
- Automatic TFLite schema extraction  
- TLV validation rules + strictness levels  

Currently functional, but **intended to evolve**.

## Wire Format

All structures are little-endian and packed.

### Patch Header (`tmd_hdr_t`)

```c
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
```

### Metadata TLVs

```
[tag][len][value...]
```

### Chunk Header (`tmd_chunk_hdr_t`)

```c
typedef struct __attribute__((packed)) {
  uint32_t off;     // offset in model
  uint16_t len;     // encoded length
  uint8_t  enc;     // 0=RAW, 1=RLE
  uint8_t  has_crc; // optional CRC32
} tmd_chunk_hdr_t;
```

## POSIX / macOS Demo Environment

The POSIX port simulates MCU flash using a file (`flash.bin`).

### Current Layout (Updated)

```c
/**
 * @file flash_layout.h
 * @brief POSIX flash layout used by example.
 *
 * Author: Felix Galindo
 * TinyMLDelta Project (Apache-2.0)
 */

static const tmd_layout_t g_layout = {
    .slotA     = { .addr = 0u,          .size = 128u * 1024u },
    .slotB     = { .addr = 128u*1024u,  .size = 128u * 1024u },
    .meta_addr = 256u * 1024u,
    .meta_size = 4 * 1024u,
};
```

- Slot A = active model  
- Slot B = inactive model  
- Metadata page = journaling for crash-safe updates  

## Running the Full End-to-End Demo

### 1. Generate realistic MCU-sized models

```bash
cd examples/modelgen
python3 make_sensor_models.py
```

Outputs:

- `base.tflite` (~67 KB)
- `target.tflite` (~67 KB, slightly different)

### 2. Create simulated flash

```bash
cd ../posix
python3 make_flash.py flash.bin
```

### 3. Generate patch

```bash
python3 ../../cli/tinymldelta_patchgen.py     ../modelgen/base.tflite     ../modelgen/target.tflite     patch.tmd     --auto-meta
```

### 4. Build + run demo

```bash
./run_demo.sh
```

It will:

✔ Build models  
✔ Generate patch  
✔ Build TinyMLDelta core  
✔ Build POSIX flash port  
✔ Write base model into both slots  
✔ Apply patch  
✔ Verify slot contents  

## Understanding the Logs

Example:

```
chunk[0]: off=62548 len=383 enc=0 has_crc=1
flash_write addr=0x0000F454 len=383
patch applied OK, new active slot=0
```

Meaning:

- Only **383 bytes** changed  
- Those bytes begin at **offset 62,548**  
- Patch applied to inactive slot  
- Active slot flipped atomically  

Verification:

```
SUCCESS: target model found at offset 0
```

---

## Directory Layout

```
TinyMLDelta/
├── cli/
│   ├── tinymldelta_patchgen.py
│   └── tinymldelta_meta_compute.py
│
├── runtime/
│   ├── include/
│   │   ├── tinymldelta.h
│   │   ├── tinymldelta_config.h
│   │   ├── tinymldelta_internal.h
│   │   ├── tinymldelta_ports.h
│   └── src/
│       └── tinymldelta_core.c
│
├── examples/
│   ├── posix/
│   │   ├── flash_layout.h
│   │   ├── tinymldelta_ports_posix.c
│   │   ├── demo_apply.c
│   │   ├── make_flash.py
│   │   └── run_demo.sh
│   └── modelgen/
│       ├── make_sensor_models.py
│
└── README.md
```

## Contributing

Contributions welcome:

- New front-ends (ONNX, Edge Impulse)
- Optimized diff algorithms
- MCU HAL ports (Zephyr, Arduino, STM32, ESP32, XBee)
- Metadata TLV extensions
- Docs improvements
- Security signing pipeline

## License

Apache-2.0  
Copyright © 2024–2025  
TinyMLDelta Project / Felix Galindo
