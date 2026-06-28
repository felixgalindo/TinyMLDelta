# TinyMLDelta — Implementation Roadmap

Consolidated change list from the architecture-update / benchmarking work. Grouped
by component and priority. Design principle throughout: **the on-device core stays
small (~2 KB), dormant outside updates, ~1 KB apply RAM, and every new capability
is opt-in** — apps that don't need a feature link/provision nothing.

Priority: **P0** correctness, **P1** architecture updates, **P2** efficiency &
security, **P3** evaluation & paper.

---

## P0 — Correctness / safety (do first; unblocks everything)

| # | Change | Where | Notes |
|---|--------|-------|-------|
| 1 | **Fix uint16 chunk-length overflow** — split any region > 65,535 B into sub-chunks (or widen `len` to u32) | PatchGen (`tinymldelta_patchgen.py` step 8) | Currently `struct.pack` crashes *after* the header is written → silently truncated/corrupt patch. Blocks all model-growth / arch measurement. |
| 2 | **Atomic/validated patch write** — write to temp + validate (re-parse, check chunk count) before finalizing; fail loudly | PatchGen | Symptom surfaced by #1: a crashed run left a 377 B "valid-looking" file. Never emit a partial patch. |
| 3 | **Base-slot digest verification** — before apply, check active slot matches header `base_chk` | Core (`tinymldelta_core.c`) | Paper Exp 6 lists "base digest mismatch detection is planned." Prevents applying a patch to the wrong base. |

## P1 — Architecture updates (the main feature)

| # | Change | Where | Notes |
|---|--------|-------|-------|
| 4 | **COPY/ADD opcode patch format** — `COPY[src_off,len]` from active slot + `ADD[literal]`; subsumes overwrite-chunks | PatchGen format + Core apply loop | Solves the FlatBuffer offset-cascade (Wall A). Device stays dumb/flat; ~+100 LOC, same 1 KB RAM. See `docs/architecture-updates.md`. |
| 5 | **Copy-matching differ** (VCDIFF/bsdiff-style suffix matching) to emit COPY ops | PatchGen | Finds shifted weight buffers automatically; model-agnostic; O(n) is fine off-device. |
| 6 | **Sequential slot-B build** from the opcode stream | Core | Retires the separate copy-then-overwrite pass; handles model growth/shrink naturally. |
| 7 | **`MAX_MODEL_BYTES` / slot-capacity check** — reject targets larger than slot B before transmit | PatchGen TLV + Core/config | So a too-big model fails up front, not mid-apply. |
| 8 | **Reframe guardrails to "accept within envelope"** — opset ⊆ linked set, arena ≤ provisioned, IO compatible | Core guardrail checks + config | Today they reject *any* change. Opset becomes a subset/superset check (not strict hash equality). See `docs/capability-envelope.md`. |
| 9 | **Capability descriptor** — firmware declares its envelope (ops, max arena, max model, I/O flexibility) | config / build | The thing PatchGen checks each patch against. |

## P2 — Efficiency & security

| # | Change | Where | Notes |
|---|--------|-------|-------|
| 10 | **In-place / copy-on-write apply (optional mode)** — write only changed sectors + small rollback journal | Core + ports | The honest flash-wear fix: A/B copy currently writes a *full model* per update regardless of patch size. True wear reduction needs single-slot in-place (trades atomicity complexity). Keep A/B as default. |
| 11 | **`SCHEMA_VERSION` opaque TLV gate** + expose carried schema blob to the app | Core | Mechanism only — opaque version compare like opset/arena; never parse schema. See `docs/io-schema-integration.md`. |
| 12 | **Optional `schema/` helper module** — reference versioned I/O schema format + PatchGen stamp flag + tiny C parser | new module *beside* core | Paved path without coupling the ~2 KB core. App owns semantics. |
| 13 | **SHA-256 / AES-CMAC / COSE signatures** | Core + PatchGen | Already on roadmap; header digest fields are already 32 B. |

## P3 — Evaluation

| # | Change | Where | Notes |
|---|--------|-------|-------|
| 14 | **Arch scenarios in bench** (add-class, widen, prune, add-layer) with **trained** models | `bench/` | Untrained models inflate diffs via zero-buffer dedup — must train. |
| 15 | **Benchmark COPY/ADD vs byte-diff vs structure-aware** on arch changes | `bench/` | Hard numbers behind the design. |
| 16 | **Finish full S1–S4 run** for clean weight/quant numbers | `bench/` | In progress. |

---

## Wire-format / API impact summary

- **New chunk/opcode type:** `COPY` (alongside `ADD`/RAW/RLE). Bump format version or
  gate by a header flag for back-compat.
- **New TLVs:** `MAX_MODEL_BYTES` (#7), `SCHEMA_VERSION` (#11). Both opaque to core.
- **Guardrail semantics:** opset hash → subset check; IO hash → compatible check (#8).
- **Core API:** apply result/callback exposes the carried schema blob to the app (#11).
- **Back-compat:** existing weight-only patches remain a degenerate case of COPY/ADD;
  keep the old applier path or emit v1 patches when no COPY ops are needed.

## What stays OUT of core (by design)

- Schema **format/semantics/parsing** → application (or optional helper module).
- FlatBuffer parsing for *bytes* → PatchGen only (device never parses the model).
- Op kernels, arena, I/O-flexible app code → integrator provisioning, not core.

## Suggested order

1, 2, 3 → 4, 5, 6 → 14, 15 (prove it) → 7, 8, 9 → 11, 12 → 17–20 (paper) → 10, 13.
