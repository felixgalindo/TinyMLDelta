# Quickstart: Run the TinyMLDelta POSIX Demo

The POSIX demo simulates the **full MCU update flow** end-to-end:

- Model generation  
- Patch generation  
- Flash image creation  
- Patch installation (A/B atomic swap)  
- Flash verification  

This allows you to validate TinyMLDelta end-to-end **without hardware**.

---

## 1. Install the TinyMLDelta CLI

From the repo root:

```bash
cd cli
./install.sh
source .tinyenv/bin/activate
```

This installs:

- TensorFlow  
- Model generator dependencies  
- PatchGen dependencies  
- A self-contained `.tinyenv` Python virtual environment  

---

## 2. Run the POSIX Demo

```bash
cd ../examples/posix
./run_demo.sh
```

---

## What `run_demo.sh` Does

`run_demo.sh` performs an MCU-style OTA update sequence:

1. Detects repo, model, and demo directories  
2. Generates `base.tflite` and `target.tflite` using TensorFlow  
3. Runs `tinymldelta_patchgen.py` to create `patch.tmd`  
4. Builds the POSIX demo app (`demo_apply`) using Clang  
5. Creates a simulated flash image (`flash.bin`) with:  
   - Slot A = Active  
   - Slot B = Inactive  
   - Metadata / journal region  
6. Applies the patch atomically to the inactive slot  
7. Verifies that Slot B matches the exact target model  

---

## Example Session (Full Output, Truncated Only for Length)

```text
[run_demo] Repo root       = /Users/felix/Projects/TinyMLDelta
[run_demo] Model dir       = /Users/felix/Projects/TinyMLDelta/examples/modelgen
[run_demo] POSIX demo dir  = /Users/felix/Projects/TinyMLDelta/examples/posix

[run_demo] Step 1: generate base.tflite and target.tflite
[ModelGen] Building base sensor model...
[ModelGen] Saved base.tflite (66368 bytes)
[ModelGen] Nudging dense2 weights to create target version...
[ModelGen] Saved target.tflite (66368 bytes)

[run_demo] Step 2: generate TinyMLDelta patch (patch.tmd)
TinyMLDelta patch written: .../examples/posix/patch.tmd
Chunks: 1  Encoded bytes: 382  Meta: 0 bytes
[run_demo] Patch size       :      474 bytes
[run_demo] Base model size  :    66368 bytes
[run_demo] Target model size:    66368 bytes

[run_demo] Step 3: build POSIX demo
clang ... -o demo_apply ...

[run_demo] Step 4: create simulated flash image
[make_flash] Created flash: .../flash.bin (262144 bytes)
[make_flash] Base model: 66368 bytes written to A and B

[run_demo] Step 5: apply patch to flash.bin
TinyMLDelta: ---- Patch Header ----
TinyMLDelta: v=1 algo=1 chunks_n=1
TinyMLDelta: base_len=66368 target_len=66368
TinyMLDelta: meta_len=0 flags=0x0000
TinyMLDelta: guardrail check
TinyMLDelta:  req_arena_bytes=0 firmware=65536
TinyMLDelta:  tflm_abi=0 firmware=1
TinyMLDelta:  opset_hash=0x00000000 firmware=0x00000000
TinyMLDelta: active slot=1 inactive=0
...
TinyMLDelta: chunk[0]: off=62728 len=382 enc=0 has_crc=1
TinyMLDelta:  flash_write addr=0x0000f508 len=382
TinyMLDelta: clearing journal
TinyMLDelta: patch applied OK, new active slot=0
Patch applied successfully.

[run_demo] Step 6: verify flash contents match target model
[verify_flash] SUCCESS: target model found at offset 0 in flash image.
```

If you see:

```
SUCCESS: target model found at offset 0 in flash image.
```

…the full differential update flow executed successfully.

---

## Manual Patch Generation (PC / CI)

If you want to use PatchGen directly, outside the POSIX demo:

```bash
cd cli
source .tinyenv/bin/activate

python3 tinymldelta_patchgen.py     ../examples/modelgen/base.tflite     ../examples/modelgen/target.tflite     ../examples/posix/patch.tmd    
```

Typical output:

```text
TinyMLDelta patch written: .../examples/posix/patch.tmd
Chunks: 1  Encoded bytes: 382  Meta: 0 bytes
```

Patch size depends on how many bytes changed between base and target.

---

## POSIX Flash Layout

The demo simulates MCU flash using `flash.bin`.

```c
static const tmd_layout_t g_layout = {
    .slotA     = { .addr = 0u,          .size = 128u * 1024u },
    .slotB     = { .addr = 128u*1024u,  .size = 128u * 1024u },
    .meta_addr = 256u * 1024u,
    .meta_size = 4 * 1024u,
};
```

### Meaning of regions:

- **Slot A** — Active model  
- **Slot B** — Inactive model  
- **Metadata / Journal** — Crash recovery, journaling, safe atomic swap tracking  

### Update flow:

1. Active Slot A → copied into Slot B  
2. Patch applied to Slot B  
3. Guardrails verified (ABI, opset hash, arena limits, I/O schema)  
4. Slot B activated  
5. Journal cleared  

This models how real MCUs perform OTA model updates safely.

---

## End of Quickstart

You are now ready to generate and apply incremental ML model updates using TinyMLDelta — entirely offline, with full reproducibility, and no embedded hardware required.
