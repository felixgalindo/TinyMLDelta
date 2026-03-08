# TinyMLDelta — Arduino UNO Q Qwiic Thermocouple Demo

End-to-end demonstration of **TinyMLDelta** on the Arduino UNO Q:
collect sensor data → train an anomaly-detection model on your PC →
push a binary patch to the live device → run inference, all without
re-flashing the full firmware.

---

## What this demo shows

| Phase | What happens |
|-------|-------------|
| **Training** | Device polls the MCP9600 thermocouple, builds a Z-score baseline, and streams a CSV of 200 temperature readings to your PC. |
| **Model generation** | `make_model.py` trains a tiny Keras autoencoder on that CSV, converts it to TFLite, and calls `tinymldelta_patchgen.py` to produce a binary `.tmd` patch. |
| **OTA update** | `send_patch.py` (or `run_demo.py`) sends the patch over the Monitor serial link. The device applies it to an inactive RAM slot, verifies it with CRC32, and atomically flips the active slot — no full re-flash. |
| **Inference** | Device runs anomaly detection on live thermocouple readings. Touch or breathe on the probe to trigger an anomaly flag. |

---

## Hardware

| Item | Notes |
|------|-------|
| **Arduino UNO Q** | STM32U585 + Linux co-processor |
| **SparkFun Qwiic MCP9600** | I²C thermocouple amplifier, address `0x60` |
| **Thermocouple probe** | K-type (included with the MCP9600 breakout) |
| **Qwiic cable** | Connects MCP9600 to the UNO Q Qwiic port |
| **USB-C cable** | Powers the board and carries the serial link |

---

## Software requirements

| Tool | Version | Install |
|------|---------|---------|
| **Arduino IDE** or **arduino-cli** | IDE 2.x / CLI 1.4+ | [arduino.cc](https://www.arduino.cc/en/software) |
| **arduino:zephyr** platform | 0.53.1+ | Board Manager → "Arduino UNO Q Board" |
| **SparkFun MCP9600 library** | any | Library Manager → "SparkFun MCP9600 Thermocouple Library" |
| **Python 3.9+** | 3.9–3.12 | [python.org](https://www.python.org) |
| **TensorFlow** | 2.x | `pip install tensorflow` |
| **NumPy** | any | `pip install numpy` |
| **pyserial** | any | `pip install pyserial` |

---

## Quick start

```bash
# 1. Install Python deps, compile & flash the sketch
./setup.sh

# 2. Run the full demo interactively
python3 run_demo.py
```

`run_demo.py` walks you through every step — close the Arduino IDE's
Serial Monitor before running it (the two tools share the serial port).

---

## Manual step-by-step

### 1. Flash the sketch

**Arduino IDE:** open `UnoQ_TinyMLDeltaDemo.ino`, select board
*Arduino UNO Q*, and click Upload.

**arduino-cli:**
```bash
arduino-cli compile --fqbn arduino:zephyr:unoq \
    --build-property "build.extra_flags=-DTMD_HAS_TFLM=0" .
arduino-cli upload  --fqbn arduino:zephyr:unoq \
    --port /dev/cu.usbmodem*  .
```

### 2. Open the Serial Monitor

Open the Arduino IDE Serial Monitor on the UNO Q port
(`/dev/cu.usbmodem…` on macOS, `/dev/ttyACM…` on Linux).
The baud rate selector doesn't affect output — the Monitor channel
is routed through the board's Linux co-processor, not a raw UART.

You should see:
```
================================================
 TinyMLDelta Qwiic Thermo Anomaly Demo
 Arduino UNO Q | MCP9600 | TinyMLDelta
================================================
```

### 3. Training mode

Type `t` and press Enter in the Serial Monitor.
The device collects 200 temperature samples at 500 ms intervals
(~100 seconds) and then prints:

```
[TRAIN] --- CSV BEGIN ---
temp_c
23.4375
23.5000
...
[TRAIN] --- CSV END ---
[TRAIN] Baseline: mean=23.45 std=0.12 C
```

Copy the CSV block (including the `temp_c` header) and save it as
`training_data.csv` in this directory.

### 4. Generate model and patch

```bash
cd examples/UnoQ_TinyMLDeltaDemo
python3 make_model.py --csv training_data.csv
```

Output files:

| File | Description |
|------|-------------|
| `base.tflite` | Autoencoder trained on your data |
| `target.tflite` | Fine-tuned variant (the update target) |
| `model.h` | C header embedding `base.tflite` for the sketch |
| `patch.tmd` | Binary TinyMLDelta patch (base → target) |

### 5. Send the patch (Update mode)

**Close the IDE Serial Monitor first** (frees the serial port), then:

```bash
python3 send_patch.py /dev/cu.usbmodem<XXXX> patch.tmd
```

The script:
1. Sends `u` to enter Update mode on the device.
2. Sends a 4-byte little-endian length prefix.
3. Streams the patch bytes.
4. Reads device responses until `[UPDATE] Done` appears.

### 6. Inference mode

Re-open the Serial Monitor and type `i`.
The device prints a reading every 500 ms:

```
[INFER]  23.44 C  score=0.012  OK
[INFER]  23.50 C  score=0.011  OK
[INFER]  41.00 C  score=0.087  *** ANOMALY ***
```

Touch or breathe on the thermocouple probe to trigger anomalies.

---

## Serial Monitor commands

| Key | Action |
|-----|--------|
| `t` | Start Training mode |
| `i` | Toggle Inference mode |
| `u` | Enter Update mode (then run `send_patch.py`) |
| `s` | Print status |
| `?` | Print menu |

---

## How TinyMLDelta works in this demo

```
PC                                  Device (STM32U585)
──────────────────────────────────────────────────────
make_model.py                       RAM slot A  [base model]
  │                                 RAM slot B  [empty]
  └─ patch.tmd ─► send_patch.py
                      │
                      └─ Monitor serial ─► tmd_apply_patch_from_memory()
                                               │
                                               ├─ Copy slot A → slot B
                                               ├─ Apply diff chunks to slot B
                                               ├─ Verify CRC32
                                               └─ Flip active slot → B
                                           Device now runs from slot B
```

The patch is typically a few hundred bytes instead of a full
model re-flash — ideal for bandwidth-constrained OTA updates.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No output in Serial Monitor | Reset the board; make sure Qwiic cable is seated |
| `[SENSOR] MCP9600 not found` | Board was powered before sensor was connected — reset it |
| `send_patch.py` can't open port | Close the Arduino IDE Serial Monitor first |
| `[UPDATE] FAILED` | Patch too large for RAM slot (kPatchBufSize = 4096 B) |
| `pip install tensorflow` fails | Use Python 3.9–3.12; TF doesn't yet support 3.13+ |

---

## Files

```
UnoQ_TinyMLDeltaDemo/
├── UnoQ_TinyMLDeltaDemo.ino      Main Arduino sketch
├── tinymldelta_ports_arduino.cpp  RAM-backed TinyMLDelta port
├── flash_layout_uno_q.h           Virtual address map for A/B slots
├── model.h                        Placeholder model (replace with make_model.py output)
├── make_model.py                  Train autoencoder, generate patch.tmd
├── send_patch.py                  Stream patch to device over serial
├── run_demo.py                    Interactive end-to-end demo runner
├── setup.sh                       Dependency installer + sketch compiler
└── README.md                      This file
```
