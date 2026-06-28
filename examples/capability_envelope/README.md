# Capability Envelope — Reference Implementation

A self-contained, runnable reference for the **capability envelope** described in
[`docs/capability-envelope.md`](../../docs/capability-envelope.md). It shows how a
firmware build declares what it can run, and how each candidate update patch is
accepted or rejected against that envelope — turning the guardrails from "reject
any architecture change" into "accept within the declared envelope, reject only on
overflow."

No TFLM or TinyMLDelta core needed — builds with any C99 compiler.

## Run

```bash
make run      # build + run the demo
make test     # same, but exits non-zero on any unexpected decision (CI)
make clean
```

## What it demonstrates

A firmware declares one envelope (linked ops, 48 KB arena, 160 KB slot, I/O
flexibility), then eight candidate patches are checked:

| Patch | Decision | Why |
|-------|----------|-----|
| weight update | ACCEPT | same arch |
| add class | ACCEPT | app reads `n_classes` at runtime |
| widen layer | ACCEPT | same ops; fits arena + slot |
| input 32→48 | ACCEPT | app reads H/W at runtime |
| add LSTM | REJECT(opset) | operator not linked into firmware |
| too big arena | REJECT(arena) | needs more activation memory than provisioned |
| oversize model | REJECT(slot) | larger than the flash slot |
| unsupported io | REJECT(io) | changes an I/O axis the app can't absorb |

## How it maps to the real system

| This example | TinyMLDelta |
|--------------|-------------|
| `tmd_envelope_t.linked_ops` | the firmware's `MicroMutableOpResolver` set |
| `tmd_envelope_t.arena_bytes` | `TMD_FIRMWARE_ARENA_BYTES` |
| `tmd_envelope_t.slot_bytes` | the A/B slot capacity |
| `tmd_patch_caps_t.req_arena_bytes` | the `REQ_ARENA_BYTES` TLV |
| `tmd_patch_caps_t.used_ops` | the `OPSET_HASH` TLV (here a subset bitmask) |
| `tmd_envelope_check()` | the runtime guardrail check (envelope-accept form) |

## Files

| File | Purpose |
|------|---------|
| `envelope.h` | envelope + patch-capability types, check API |
| `envelope.c` | the accept/reject logic |
| `main.c` | firmware envelope + eight cases, with assertions |
| `Makefile` | build / run / test / clean |
