# Architecture Updates: Why Byte-Delta Isn't Enough (and the Path Forward)

**Motivation:** to be practical, TinyMLDelta can't depend only on weight-delta
updates — real model improvement changes the *architecture* (add a class,
widen/prune layers, add a layer, change input resolution). The current design
explicitly *rejects* these (the OPSET/IO/ARENA guardrails exist to reject arch
changes). This doc records what we measured and the design that addresses it.

## What we measured (ResNet-8, CIFAR-10, float32)

| Change | Logical size Δ | Byte-level diff | PatchGen result |
|--------|----------------|-----------------|-----------------|
| Weight update (trained→trained, `run_bench` S2) | ~0 | **0.6%** | ✅ small patch |
| Input resolution 32→48 (no buffer resized) | +24 B | **1.0%** | ✅ small patch |
| **Add class 10→11** (output buffer grows) | +272 B | **98.3%** (309,712 B region) | ❌ **crash** |
| **Widen stack 64→96 filters** | +227 KB | **99.9%** (538,844 B region) | ❌ **crash** |

Two distinct failures show up:

### 1. A wire-format bug (must fix first)
The chunk `len` field is **uint16** (`CHUNK_FMT = "<IHBb"`), capped at 65,535 B.
Any contiguous diff region larger than 64 KB makes `struct.pack` throw
*after* the header is written, leaving a **silently truncated, corrupt patch**.
Every architecture change above tripped this. Fix: split regions into ≤64 KB
sub-chunks (or widen the field to uint32). Until fixed, arch changes can't even
be *measured*.

### 2. The real problem: FlatBuffer offset cascade
TFLite serializes weights as length-prefixed buffers in a buffer table, and
**deduplicates identical buffers**. Any change that *resizes a buffer* (adding a
class grows the output Dense buffer; widening grows conv buffers) shifts the
offsets of every buffer after it → ~100% of the byte stream differs even though
the *semantic* change is tiny. Byte-level diffing is structurally the wrong tool
for architecture updates.

> Caveat learned the hard way: comparing *untrained* models also inflates diffs,
> because zero-initialized biases/BN-betas get deduplicated and a tiny change
> breaks the dedup. Always benchmark architecture deltas with **trained** models.

Shape-only changes that resize *no* buffer (e.g., input resolution, since
GlobalAveragePool makes the classifier resolution-independent) stay small and
**are** patchable today.

## Two walls, not one

| Wall | What it is | Fixable by a better diff? |
|------|-----------|---------------------------|
| **A. Representation** | byte-diff explodes on FlatBuffer reshuffle | **Yes** — structure-aware diffing |
| **B. Runtime** | new ops need linked kernels; arena is fixed at compile time; I/O schema is app-coupled | **Partially** — over-provisioning; otherwise needs firmware |

## Path forward

### Mode 2 — structure-aware (graph-level) diffing
Parse both TFLite FlatBuffers, align at the **tensor/buffer/operator** level (not
bytes), and emit a patch as graph-edit ops:
`replace_buffer(id, bytes)`, `resize_tensor(id, shape)`, `insert_op(...)`,
`set_output_dim(...)`. The runtime reconstructs the target FlatBuffer in the
inactive slot from base + edit script. Immune to offset cascades; patch size
tracks the *actually changed* tensors. This is the headline new contribution and
turns "we reject arch changes" into "we support them within a capability envelope."

### Capability-envelope firmware (the practical enabler)
Ship firmware **over-provisioned**:
- **Superset op resolver** — link every op you might ship (or `AllOpsResolver`).
- **Arena sized for the largest expected model** (with declared head-room).
- TFLM already reads tensor **shapes from the model** at `AllocateTensors()`, so
  width/depth/resolution/added-class changes need *no* recompile **as long as**
  (a) the ops are already linked and (b) it fits the arena.

Reframe the guardrails from "reject any architecture change" to "reject only what
exceeds the declared envelope": a **new operator type**, an **arena overflow**, or
an **I/O-schema break**. Everything inside the envelope becomes a patchable
architecture update. The patch carries a capability descriptor; PatchGen checks
the target against the firmware's envelope and accepts/rejects accordingly.

### The honest hard wall
Genuinely **new operator types** or **arena overflow** require a firmware OTA on
bare-metal MCUs — that's MCUboot/Mender territory and out of scope. *But* on
Linux-class edge targets (e.g., the UNO Q's aarch64 side, where the current demo
already runs inference), even new ops can ship as a shared-library / delegate
update — so TinyMLDelta + architecture updates is most compelling there.

### Slot sizing
The copy-then-overwrite design copies the active slot into the inactive slot then
overwrites diff chunks. A *growing* model needs the inactive slot provisioned for
the **largest** model in the envelope, not just the current one. Document this as
a deployment requirement.

## Benchmark additions (to substantiate the above)
1. Fix the uint16 chunk bug; re-measure all arch changes with **trained** models.
2. Add scenarios: add-class, prune-channels, widen, add-layer, input-resolution.
3. Report **byte-diff vs structure-aware-diff** patch size side by side — the
   structure-aware patch should track changed-tensor bytes while byte-diff ≈100%.
4. Report what fraction of each arch change fits a reasonably-provisioned envelope.

## Takeaway
The defensible position is **not** "architecture changes require a full firmware
update." It's: **weight + many architecture updates are patchable within a
capability envelope via structure-aware diffing; only new operator types or arena
overflow truly need firmware.** See [capability-envelope.md](capability-envelope.md)
for the integrator-side provisioning that makes this real.
