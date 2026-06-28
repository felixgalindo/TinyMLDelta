# Scaling Beyond TFLite: Multi-Format Future-Compatibility

TinyMLDelta's core diff/apply engine is **already format-agnostic** — it operates
on raw bytes (and, with COPY/ADD, on byte ranges), so it can delta *any* binary.
What is TFLite-specific today is the **safety/compatibility metadata** (opset,
arena, I/O, ABI guardrails) and the metadata *extraction*. Scaling past TFLite is
therefore not a core rewrite — it is making the **compatibility layer pluggable**
and declaring **what kind of artifact** a patch targets.

## Three layers (the key separation)

| Layer | Owns | Format coupling |
|-------|------|-----------------|
| **Transport / integrity** | byte / COPY-ADD delta, CRC32 / SHA-256 | **none** — works on any bytes |
| **Compatibility** | "can this device *apply + load* this?" — format id, format version, opset/arena/IO/ABI guardrails | **per-format** (pluggable) |
| **Semantic (I/O schema)** | "how does the *app* drive the model?" — labels, normalization, task, threshold | format-independent; app-owned |

The question "should model version / format type be in the schema?" resolves
cleanly with this split:

> **No — format identity and version belong in the *compatibility* layer (patch
> TLVs), not the app-facing I/O schema.** The I/O schema describes the model's
> *interface*; the format/version describes *what the artifact is and whether the
> runtime can load it*. Different concerns, different layers. (The core carries
> both — the I/O schema opaquely, the format/guardrail TLVs as typed values it
> compares against firmware capability.)

## Two classes of "model format"

### Class A — loadable blobs (delta applies directly)
Interpreter loads a data artifact. TinyMLDelta deltas the bytes; only the metadata
extractor differs per format.

| Format | Container | Notes |
|--------|-----------|-------|
| **TFLite / TFLite Micro** | FlatBuffer | current; offset-cascade on resize → COPY/ADD helps |
| **ONNX / ORT** | Protobuf | varint + field ordering → *less* byte-stable than FlatBuffer; **structure-aware diff matters more** |
| **ExecuTorch (.pte)** | FlatBuffer | PyTorch on-device; similar to TFLite |
| **GGUF** (llama.cpp) | flat header + tensors | tiny-LLM edge; clean tensor layout → great for structure-aware diff + LoRA deltas |
| **NNEF** (Khronos) | container | exchange format |

### Class B — compiled / codegen (only weights are patchable)
A compiler inlines the graph into C/object code; the "model" is partly *code* +
weights. Only a **separable weight/params blob** is patchable; a graph change
means recompiling firmware → out of scope (that's firmware OTA).

| Format | Output | Patchable part |
|--------|--------|----------------|
| **Edge Impulse EON** | C++ source / TFLite | params blob if separated |
| **microTVM / TVM AOT** | generated C + params | params only |
| **STM32Cube.AI / X-CUBE-AI** | C + weights | weight section |
| **CMSIS-NN / hand-written** | C arrays | weight arrays |

**Recommendation for Class B:** define a *params-blob convention* (weights in a
contiguous, separately-addressable region) so TinyMLDelta can patch weights even
when the graph is compiled in. Architecture changes remain firmware updates.

## What we need to add to be future-compatible

1. **`MODEL_FORMAT` TLV** (u8/u16 enum: raw, tflite, onnx, executorch, gguf,
   eon-params, …) + **`FORMAT_VERSION` TLV** (the format's own version — TFLite
   schema, ONNX opset, GGUF version). Compatibility layer, **not** the I/O schema.
2. **Patch-format version** — header `v` (bump for COPY/ADD) + a `flags`
   capability bit so devices negotiate opcode support.
3. **Pluggable PatchGen metadata extractors** — one per format produces the
   guardrail TLVs; the core only *compares* opaque values against firmware config,
   staying format-agnostic.
4. **Format axis in the capability envelope** — firmware declares supported
   formats + versions; PatchGen rejects a patch for an unsupported format/version
   *before* transmission (a device linking only TFLM rejects `MODEL_FORMAT=onnx`).
5. **Structure-aware (COPY/ADD) diff** — already the plan; it is *more* important
   for protobuf (ONNX) than FlatBuffer, since protobuf is less byte-stable.

## Why this is the right architecture (and a contribution)

Formalizing the **transport / compatibility / semantic** layering — with format
identity + version + guardrails in a declarative compatibility envelope, and the
I/O schema kept separate and app-owned — generalizes on-device model updatability
*across formats and runtimes*, not just TFLite. The byte/COPY-ADD engine and
integrity stay universal; only a thin, pluggable per-format extractor is new.

## High-value future targets

- **GGUF / tiny LLMs** — frequent LoRA/adapter fine-tunes, clean tensor layout →
  delta patching is especially compelling (small structured deltas).
- **ONNX / ExecuTorch** — broad runtime reach; structure-aware diff unlocks them.
