# Supporting Architecture Updates: The Capability Envelope

TinyMLDelta patches model **bytes** in flash. By default it patches *weight-only*
updates (same architecture) and **rejects** architecture changes via its guardrail
TLVs (`OPSET_HASH`, `REQ_ARENA_BYTES`, `IO_HASH`). That default is intentional:
on bare-metal, a model that uses an unlinked operator, overflows the arena, or
breaks the firmware's I/O contract simply will not run.

But most of that wall is **provisionable**. If you *want* your fleet to receive
architecture updates in the field (added classes, widened/pruned layers, new
input resolution, extra layers using existing ops), you opt in by giving the
firmware **head-room** up front — a **capability envelope** — and TinyMLDelta will
then accept any update that stays inside it.

This is opt-in. Deployments that never need architecture updates provision none of
this and pay nothing — the runtime stays ~2 KB, dormant, 1 KB-RAM.

> **Two parts to an architecture update.** Getting the new *bytes* into flash is
> the patch format's job (a COPY/ADD opcode stream — see
> [`architecture-updates.md`](architecture-updates.md)). Being able to *run* the
> new architecture is this document's job: provisioning the envelope.

---

## The four levers

Each lever widens the envelope along one axis and unlocks a class of update.
Provision the ones whose update classes you care about; skip the rest.

| Lever | Unlocks | Cost | Where |
|-------|---------|------|-------|
| 1. Superset op resolver | new operator *types* | flash (10s KB) | firmware build |
| 2. Over-sized arena | wider/deeper/higher-res models | RAM (linear) | firmware build |
| 3. Slot sized for the largest model | any model *growth* | flash (1× max model) | flash map |
| 4. Schema-flexible I/O | added classes, new resolution/quant | app discipline | app code |

---

### 1. Superset op resolver — link the ops you might ship

TFLM links operator kernels at compile time via a statically-sized resolver. A
model that uses an unlinked op fails at `AllocateTensors()`. Provision by linking
a **superset** of the ops your roadmap might use, not just today's:

```cpp
// Curated TinyML superset — covers CNN / DS-CNN / MobileNet / autoencoder
// iterations. Size the template param to the count.
static tflite::MicroMutableOpResolver<20> resolver;
resolver.AddConv2D();          resolver.AddDepthwiseConv2D();
resolver.AddFullyConnected();  resolver.AddReshape();
resolver.AddMaxPool2D();       resolver.AddAveragePool2D();
resolver.AddMean();            // global average pool
resolver.AddSoftmax();         resolver.AddLogistic();
resolver.AddRelu();            resolver.AddRelu6();
resolver.AddAdd();             resolver.AddMul();
resolver.AddConcatenation();   resolver.AddPad();
resolver.AddQuantize();        resolver.AddDequantize();
resolver.AddStridedSlice();    resolver.AddTranspose();
resolver.AddMaximum();
```

- **Guidance:** start from the union of operators across every model you expect to
  ship in the next ~year. A ~15–20 op set covers the large majority of
  CNN-family iterations. Use `AllOpsResolver` only if flash is plentiful (it links
  every builtin — tens of KB).
- **Enforcement:** PatchGen stamps `OPSET_HASH` from the target model; set
  `TMD_FIRMWARE_OPSET_HASH` to a hash of your linked set (or `0` to disable the
  check and rely on the resolver failing safe at `AllocateTensors()`).
- **Cost:** flash only (code). No RAM, no inference cost.
- **Limit:** a genuinely novel op you didn't link still needs a firmware OTA. On
  Linux-class targets, ship it as a shared-lib/delegate instead.

---

### 2. Over-sized tensor arena — provision for the biggest activations

The arena holds *intermediate activations* and is a fixed-size array. A wider,
deeper, or higher-resolution model needs more scratch and fails to allocate if it
exceeds the compiled size.

**Size it empirically across your candidate models, then add head-room:**

```cpp
// Provision for the largest model in the envelope + 30% head-room.
constexpr int kArenaSize = 48 * 1024;          // measured max ~36 KB
alignas(16) static uint8_t tensor_arena[kArenaSize];
```

How to find the max:

```cpp
// After AllocateTensors() succeeds for each candidate model:
size_t used = interpreter.arena_used_bytes();   // peak for THIS model
// Take the max across all candidate/roadmap models; add 25–50%; align.
```

- **Guidance:** measure `arena_used_bytes()` for each model you might deploy, take
  the max, add 25–50% head-room, round to alignment. Re-check whenever you add a
  bigger model to the roadmap.
- **Enforcement:** set `TMD_FIRMWARE_ARENA_BYTES` to `kArenaSize`. PatchGen emits
  `REQ_ARENA_BYTES` (estimated; tune with `--arena-factor`); the runtime rejects
  any patch whose requirement exceeds the firmware's arena. **This plumbing
  already exists** — you're just provisioning generously instead of tightly.
- **Cost:** RAM, permanently reserved (the real cost on small-SRAM parts).
- **Limit:** bounded by physical SRAM.

---

### 3. Slot sized for the largest model — let models grow

A/B updates require slot B to hold a **complete** target image. A growing model
needs **both** slots sized for the *largest* model in the envelope, not today's.

```c
/* Size each slot for the largest model you will ever ship + margin, aligned
 * to the flash erase sector. Both slots are equal. */
#define TMD_MAX_MODEL_BYTES   (160u * 1024u)            /* roadmap maximum     */
#define TMD_SLOT_BYTES        (((TMD_MAX_MODEL_BYTES + TMD_SECTOR_SZ - 1) \
                                / TMD_SECTOR_SZ) * TMD_SECTOR_SZ)  /* sector-aligned */
```

- **Guidance:** flash budget ≈ `2 × TMD_SLOT_BYTES + journal sector + firmware`.
  Pick `TMD_MAX_MODEL_BYTES` from your roadmap, not your current model, so you
  don't have to re-lay-out flash later.
- **Enforcement (suggested addition):** declare the slot capacity so PatchGen can
  reject an over-size target *before* transmission rather than failing mid-apply.
  A new `MAX_MODEL_BYTES` TLV (or reuse `target_len` vs a compiled
  `TMD_SLOT_BYTES` check in the runtime) closes this gap.
- **Cost:** flash — one extra max-model slot vs. a non-updatable, single-image
  build. Same A/B doubling every safe OTA scheme pays.

---

### 4. Schema-flexible I/O — stop hard-coding the contract

Even when the interpreter runs the new model, app code that hard-codes input
shape, quantization, or class count will silently feed/Read garbage. Make the app
**read the contract from the tensors at runtime:**

```cpp
// Instead of hard-coded [1,49,10,1] int8 and 12 classes:
auto* in  = interpreter.input(0);
const int H = in->dims->data[1], W = in->dims->data[2];
const float in_scale = in->params.scale;
const int   in_zp    = in->params.zero_point;

auto* out = interpreter.output(0);
const int n_classes = out->dims->data[out->dims->size - 1];   // not a literal
const int top = argmax(out->data.int8, n_classes);            // adapts to growth
```

- **Unlocks:** added output classes (read `n_classes`), input-resolution changes
  (read `H`/`W`), and quantization changes (read `scale`/`zero_point`) — all
  without a recompile.
- **Label semantics:** what class index 12 *means* still needs a label map. Ship
  it inside the model's metadata buffer or a TinyMLDelta vendor TLV (tag ≥ `0x80`)
  so it updates atomically with the model.
- **Limit (app-physical):** you can't exceed what the sensor/feature pipeline
  produces — a model wanting 64×20 MFCC frames when your front-end emits 49×10
  needs a pipeline change, which is application work, not a model patch.
- **Enforcement:** with flexible I/O, relax `TMD_ENFORCE_IO_HASH` from "exact
  match" to "compatible within declared flexibility" for the axes you support.
- **Deep dive:** when updates change I/O, the label map / normalization /
  thresholds are *not* in the tensor metadata and need a schema that travels with
  the model — see [io-schema-integration.md](io-schema-integration.md).

---

## Provisioning checklist

To enable field architecture updates on a deployment:

- [ ] **Ops:** link a superset op resolver covering your roadmap; set/curate
      `TMD_FIRMWARE_OPSET_HASH`.
- [ ] **Arena:** measure `arena_used_bytes()` across candidate models; set
      `TMD_FIRMWARE_ARENA_BYTES` = max + 25–50% head-room.
- [ ] **Slots:** size `TMD_SLOT_BYTES` for the largest roadmap model + margin,
      sector-aligned; budget `2×` for A/B.
- [ ] **I/O:** read shapes/quant/class-count at runtime; ship a label map in
      metadata/TLV; relax `IO_HASH` to the flexibility you actually support.
- [ ] **Declare the envelope** (ops set, max arena, max model size, I/O
      flexibility) so PatchGen accepts/rejects each patch against it.

## Worked example (Cortex-M4, 1 MB flash / 256 KB SRAM)

| Decision | Value | Rationale |
|---|---|---|
| Op resolver | 18-op superset | covers CNN/DS-CNN/autoencoder roadmap |
| `TMD_FIRMWARE_ARENA_BYTES` | 48 KB | max measured 36 KB + ~33% |
| `TMD_SLOT_BYTES` | 160 KB | largest roadmap model 150 KB, sector-aligned |
| Flash for models | 320 KB | 2 × 160 KB (A/B) |
| I/O | shape/quant/n_classes read at runtime | added-class + resolution updates |

Inside this envelope, the fleet can receive: weight updates, added classes,
width/depth changes, and resolution changes — all as TinyMLDelta patches, no
re-flash. Outside it (a brand-new op type, a >160 KB model, or an arena
>48 KB) still requires a firmware update — and the runtime rejects such patches
up front instead of bricking.

## What remains genuinely firmware-only

After all four levers: a **new operator type** you didn't link, a model that
**overflows the provisioned arena or slot**, or an **I/O change the sensor/app
can't satisfy**. That is the irreducible "needs firmware OTA" set — and on
Linux-class edge targets even the op-type case dissolves via dynamic loading.
