# I/O Schema Integration: Keeping the App in Sync When Updates Change I/O

A model is one half of a contract; your firmware is the other half. The moment a
model update can change **input or output**, hard-coded app code feeds the
interpreter wrong-shaped data or misreads its output — and it fails *silently*
(no crash, just garbage). This guide shows how to ship an **I/O schema** alongside
the model so the app re-syncs automatically on every update.

This is the deep-dive on lever #4 of the
[capability envelope](capability-envelope.md). Read that first for the big picture.

---

## Whose responsibility: mechanism vs. policy

**The schema's content is application-level. TinyMLDelta only carries it.**
Think *MCUboot : firmware :: TinyMLDelta : model* — the updater delivers and
verifies bytes and gates compatibility; it never parses your app's config.

| Concern | Owner |
|---------|-------|
| Schema **format, fields, semantics** (labels, `mean/std`, threshold, task) | **Application** |
| Schema **parsing / validation / decisions** | **Application** |
| **Carrying** the schema so it swaps atomically with the model | **TinyMLDelta** — *opaquely* |
| **Version gating** (is this schema version supported?) | **TinyMLDelta** — opaque compare, like `OPSET_HASH`/`ARENA` |

What this means in practice:
- **Prefer embedding the schema in the TFLite model `metadata` buffer.** Then it's
  just model bytes — patched atomically with the weights, **zero TinyMLDelta
  involvement**, impossible to desync.
- **Otherwise use an opaque vendor TLV** (tag ≥ `0x80`): core carries and CRCs the
  blob, hands the app a pointer, and never looks inside.
- Core may add only a **schema-agnostic** `SCHEMA_VERSION` TLV gate (an opaque
  number compare) and expose the blob to the app — mechanism, not policy.
- A reference schema format + parser ships as an **optional helper module beside
  core**, never inside the ~2 KB core. Apps that don't need a schema include
  nothing.

The rest of this doc is the *application-side* convention you build on top.

---

## The key distinction: what the model tells you vs. what it doesn't

When you need to handle an I/O change, split the contract into two layers:

### Layer 1 — Tensor contract (free from the interpreter, no schema needed)
Shapes, dtypes, and quantization parameters are **already in the model** and TFLM
exposes them at runtime. Read them; never hard-code them:

```cpp
auto* in  = interpreter.input(0);
const int H = in->dims->data[1], W = in->dims->data[2], C = in->dims->data[3];
const TfLiteType  itype = in->type;                 // kTfLiteInt8 / kTfLiteFloat32
const float in_scale = in->params.scale;            // int8 quant
const int   in_zp    = in->params.zero_point;

auto* out = interpreter.output(0);
const int n_classes = out->dims->data[out->dims->size - 1];   // NOT a literal
```

If your only I/O changes are shape / dtype / quant, **Layer 1 is enough** — read
at runtime and you're done.

### Layer 2 — Semantic & preprocessing contract (NOT in the tensors → needs a schema)
These are **not derivable** from the flatbuffer and must be shipped explicitly:

| What | Example | Why it's not in the model |
|------|---------|---------------------------|
| **Label map** | output idx 11 → `"forklift"` | tensors have no class names |
| **Normalization** | `mean=25.1, std=3.0` z-score | preprocessing happens *before* the model |
| **Windowing / framing** | window=4, hop=1, MFCC bins | feature extraction is app-side |
| **Task + decision rule** | anomaly, `threshold=0.04`; or top-1 classify | output is just numbers |
| **Units / scaling** | output is °C × 100 | semantics live outside the graph |

When an update changes any of these, the app must learn the new values — that's
what the schema carries.

> **Concrete example from this repo:** the UNO Q demo already ships a
> `model_config.json` with `{mean, std, window_size}` and an anomaly threshold.
> That *is* an I/O schema (the Layer-2 preprocessing contract). This guide
> generalizes it and fixes its main weakness: it's a **separate sidecar that can
> desync** from the model. Prefer carrying the schema *with* the model (below).

---

## Where to carry the schema (so it can't desync)

| Option | Travels with | Atomic w/ model? | Recommendation |
|--------|-------------|------------------|----------------|
| **Model metadata buffer** (TFLite `Model.metadata`) | the model bytes | ✅ yes — patched atomically | **Preferred** — impossible to desync |
| **TinyMLDelta vendor TLV** (tag ≥ `0x80`) | the `.tmd` patch | ✅ applied with the patch | Good when you can't touch model export |
| **Separate sidecar file** (`model_config.json`) | pushed alongside | ❌ can desync | Avoid for fields that change with the model |

Embedding in the **model metadata** is best: the schema is part of the bytes
TinyMLDelta patches, so it updates in the same atomic slot-flip as the weights —
the app can never load new weights with an old schema.

---

## A concrete schema

Keep it small and **versioned**. JSON for clarity; use CBOR/a packed struct
on-device if flash/parse cost matters.

```json
{
  "schema_version": 2,
  "input": {
    "shape": [1, 4],
    "dtype": "float32",
    "preprocess": { "type": "zscore", "mean": 25.1, "std": 3.0, "window": 4 }
  },
  "output": {
    "task": "anomaly",
    "decision": { "metric": "mse", "threshold": 0.04 }
  }
}
```

Classification variant:

```json
{
  "schema_version": 5,
  "input":  { "shape": [1,49,10,1], "dtype": "int8",
              "preprocess": { "type": "mfcc", "bins": 10, "frames": 49 } },
  "output": { "task": "classification",
              "labels": ["silence","unknown","yes","no","up","down","left"] }
}
```

`schema_version` lets the app refuse a schema it doesn't understand (e.g., a new
`preprocess.type` it has no code for) instead of mis-decoding.

---

## The app pattern: re-sync on every apply

On boot **and** after each patch apply (the active slot just flipped):

```text
1. Load the schema (from model metadata / TLV).
2. Check schema_version is one this firmware supports  → else reject, keep old slot.
3. Validate the schema against the live tensor contract (Layer 1):
     schema.input.shape == interpreter.input(0)->dims ?
     schema.output task/size consistent with output tensor ?
   Mismatch → reject (the schema and model disagree → corrupt/incompatible).
4. Configure the app from the schema:
     - preprocessing: normalization constants, window/hop, feature type
     - output handling: label map, task, decision threshold
5. Confirm the app can physically satisfy the input:
     can the sensor/feature pipeline produce this shape/feature?  → else reject.
6. Resume inference with the new contract.
```

Because TinyMLDelta is A/B with atomic flip, a **rejection here is safe** — the
previous model/schema stays active. Wire schema validation into the same accept/
reject path as the guardrails.

---

## What an I/O schema lets you absorb — and what it can't

**Absorbable with a schema, no recompile:**
- Added/renamed output classes (label map grows)
- Changed normalization / threshold / windowing constants
- Changed input resolution or quant params — *if* the feature pipeline can produce them
- A model that emits the same task with different scaling/units

**NOT absorbable by a schema (needs app or firmware change):**
- A new input **modality** (the schema can't conjure a sensor/feature pipeline)
- A new **dtype** the app has no code path for
- A changed **number of inputs/outputs** the control flow isn't written for
- A new **task type** (classification → detection/regression) — different output
  handling logic, not just different constants

The schema's job is to make the *parameterizable* parts of the contract
data-driven. Structural changes to the app's logic remain app work — but the
schema at least lets the device **detect and safely reject** them rather than run
on a broken contract.

---

## Enforcement: tie the schema to the guardrails

- Add a **`SCHEMA_VERSION`** (and optionally `SCHEMA_HASH`) TLV so PatchGen can
  reject a patch whose schema version the target firmware doesn't support, before
  transmission.
- With a schema in place, relax `TMD_ENFORCE_IO_HASH` from "exact tensor match"
  to "schema-compatible": accept I/O changes along the axes your app reads
  dynamically (shape/quant/class-count), reject the rest.

## Checklist

- [ ] Move Layer-1 reads (shape/dtype/quant/n_classes) to **runtime**, not literals.
- [ ] Define a **versioned schema** for the Layer-2 params your updates may change
      (labels, normalization, window, task, threshold, units).
- [ ] Carry it in **model metadata** (preferred) or a **vendor TLV**; retire
      desync-prone sidecars for update-coupled fields.
- [ ] On apply/boot: load → version-check → validate vs tensors → configure →
      confirm pipeline can satisfy input → else **reject and keep old slot**.
- [ ] Add `SCHEMA_VERSION` TLV; relax `IO_HASH` to schema-compatible.
