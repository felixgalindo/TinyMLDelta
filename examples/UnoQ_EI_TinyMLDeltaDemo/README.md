# TinyMLDelta — Arduino UNO Q + Edge Impulse Demo (Stub)

> **Status: Work in progress.** The sketch structure and TinyMLDelta integration are complete. The Edge Impulse inference calls are stubbed pending EI library availability on arduino:zephyr. The Z-score anomaly fallback runs today.

This example mirrors the [UnoQ_TinyMLDeltaDemo](../UnoQ_TinyMLDeltaDemo/README.md) but replaces the hand-rolled TFLite Micro autoencoder with an **Edge Impulse exported library** for anomaly detection.

---

## What it demonstrates

- Same three-mode state machine (TRAINING → UPDATE → INFERENCE) as the TFLite demo
- TinyMLDelta patch receive + apply over the Monitor serial channel
- Z-score anomaly detection fallback (runs today without EI library)
- Drop-in replacement path for EI-trained models via TinyMLDelta patches

---

## Hardware

Same as the main UNO Q demo:

| Item | Notes |
|------|-------|
| Arduino UNO Q | STM32U585 + Linux co-processor |
| SparkFun Qwiic MCP9600 | Thermocouple amplifier, address `0x60` |
| Thermocouple probe | K-type |
| Qwiic cable | UNO Q Qwiic port → MCP9600 |

---

## Edge Impulse integration (when ready)

1. Collect training CSV in Training mode (type `t` in the Serial Monitor).
2. Upload the CSV to your Edge Impulse project.
3. Train an anomaly detection block (K-means or GMM) in EI Studio.
4. Export as **Arduino library**, install in the IDE.
5. In `UnoQ_EI_TinyMLDeltaDemo.ino`, uncomment the EI include and fill in the `TODO` blocks.
6. When you retrain in EI Studio, use `make_model_ei.py` (coming soon) to diff the exported model bytes and generate a `.tmd` patch.
7. Push the patch with `send_patch.py` — no re-flash required.

---

## Serial commands

| Key | Action |
|-----|--------|
| `t` | Start Training mode (collect CSV) |
| `i` | Toggle Inference mode |
| `u` | Enter Update mode (await patch from `send_patch.py`) |
| `s` | Print status |
| `?` | Print menu |

---

## Files

```
UnoQ_EI_TinyMLDeltaDemo/
├── UnoQ_EI_TinyMLDeltaDemo.ino      Main sketch (EI stubs marked TODO)
├── tinymldelta_ports_arduino.cpp     RAM-backed TinyMLDelta port (shared with TFLM demo)
├── flash_layout_uno_q.h              Virtual A/B slot map (shared with TFLM demo)
└── README.md                         This file
```

---

## See also

- [UnoQ_TinyMLDeltaDemo](../UnoQ_TinyMLDeltaDemo/README.md) — fully working TFLM + TinyMLDelta demo
- [POSIX demo](../posix/README.md) — no-hardware simulation of the full update flow
