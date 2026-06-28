# I/O Schema — Reference Implementation

A self-contained, runnable reference for the **I/O schema** described in
[`docs/io-schema-integration.md`](../../docs/io-schema-integration.md). It shows
the *application side*: when a model update can change I/O, the app re-syncs to
the new contract instead of running on stale assumptions.

Mechanism vs. policy: TinyMLDelta carries the schema **opaquely** (in the TFLite
model metadata buffer or a vendor TLV) and gates by version; the **app** owns the
schema format and semantics. This example is that app-side owner.

No TFLM needed — builds with any C99 compiler.

## Run

```bash
make run      # build + run the demo
make test     # same, but exits non-zero on any unexpected decision (CI)
make clean
```

## What it demonstrates

The app understands schema ≤ v3, an MFCC pipeline up to 49×13, int8 + float32.
Six model updates arrive (schema + the live tensor contract); the app re-syncs:

| Update | Decision | Why |
|--------|----------|-----|
| baseline v2 | ACCEPT | configures 4 classes |
| add class (4→5) | ACCEPT | reads `n_classes` from the tensor → 5 classes |
| renorm anomaly | ACCEPT | new mean/std + threshold 0.04 |
| future schema (v7) | REJECT(version) | newer than the app understands |
| label mismatch | REJECT(tensor-mismatch) | schema says 5 labels, output tensor has 4 |
| bigger input (64×64) | REJECT(pipeline) | MFCC pipeline can't supply it |

A rejection is safe: with A/B atomic flip, the previous model/schema stays active.

## The two layers

| Layer | Source | In this example |
|-------|--------|-----------------|
| Tensor contract (shape/dtype/quant) | the interpreter, at runtime | `tmd_tensor_contract_t` |
| Semantic + preprocessing (labels, normalization, task, threshold) | carried *with* the model | `tmd_io_schema_t` |

## Files

| File | Purpose |
|------|---------|
| `schema.h` | schema + tensor-contract + app-capability types, apply API |
| `schema.c` | version-gate → tensor validation → pipeline check → configure |
| `main.c` | six update cases with assertions |
| `Makefile` | build / run / test / clean |
