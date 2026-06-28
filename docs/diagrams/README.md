# Design Diagrams (draw.io)

Editable `.drawio` source diagrams for TinyMLDelta. Open them in:
- [diagrams.net](https://app.diagrams.net) (File → Open), or
- the **Draw.io Integration** VS Code extension (`hediet.vscode-drawio`) — edit
  inline in the IDE.

Export PNG/SVG from there for the README or slides (keep the `.drawio` as source).

## Diagrams

| File | Shows |
|------|-------|
| [`system-architecture.drawio`](system-architecture.drawio) | PatchGen (off-device) → OTA transport → Core runtime → A/B flash slots, and the separate inference engine reading the active slot. |
| [`patch-apply-flow.drawio`](patch-apply-flow.drawio) | The on-device apply path: parse → guardrails/envelope → copy A→B → apply+verify chunks → digest check → atomic flip, with crash/power-loss resume. |

| [`wire-format.drawio`](wire-format.drawio) | The `.tmd` layout: header fields, TLV metadata, and chunk records (RAW/RLE). |

## Planned

- `copy-add-format.drawio` — COPY/ADD opcode stream vs. positional overwrite (see [../architecture-updates.md](../architecture-updates.md)).
- `capability-envelope.drawio` — the four provisioning levers and what each unlocks (see [../capability-envelope.md](../capability-envelope.md)).
- `io-schema-lifecycle.drawio` — schema carried in model metadata, app re-sync on apply (see [../io-schema-integration.md](../io-schema-integration.md)).

## Convention
- One concept per file; keep them small and legible.
- `.drawio` is the source of truth; commit exported images alongside only if
  referenced by docs.
