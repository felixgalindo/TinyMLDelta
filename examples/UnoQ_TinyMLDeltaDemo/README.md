# TinyMLDelta — Arduino UNO Q Temperature Anomaly Demo

End-to-end demonstration of **TinyMLDelta** on the Arduino UNO Q:
collect real sensor data, train an anomaly-detection model on your PC,
push a binary patch to the live device, and run inference — all without
re-flashing firmware.

**All application logic runs on the UNO Q Linux co-processor** (a C++
binary).  The STM32/Zephyr sketch is a thin sensor proxy that reads the
**Arduino Modulino Thermo** (HS3003) temperature sensor and forwards
data + commands to Linux via `Bridge.call()`.

Two inference variants are provided — pick one:

| Variant | Directory | Inference engine | Notes |
|---------|-----------|-----------------|-------|
| **TFLite C API** | `tflite/` | `libtensorflowlite_c.so` (v2.17) | Simpler; downloads shared lib automatically |
| **Edge Impulse SDK** | `edgeimpulse/` | EI `run_classifier()` with TFLite Micro compiled in | Uses EI's external model loading hook for TinyMLDelta integration |

Both variants share the same Arduino sketch, training script, model
generation, and shared headers (`common/`).

---

## What this demo shows

| Phase | What happens |
|-------|-------------|
| **Training** | Linux collects 200 temperature readings from the Modulino Thermo sensor and saves a CSV on-board. |
| **Model generation** | `make_model.py` trains a tiny autoencoder on the CSV, converts to TFLite, and generates a binary `.tmd` patch via `tinymldelta_patchgen.py`. |
| **OTA update** | The patch is ADB-pushed to the board. Sending `u` triggers the app to apply the patch with `tmd_apply_patch_from_memory()` and reload the TFLite interpreter — no STM32 re-flash needed. |
| **Inference** | Linux runs anomaly detection on live temperature readings. Warm or cool the sensor to trigger an anomaly flag. |

---

## Architecture

```
                        Arduino UNO Q
         ┌──────────────────────────────────────────────┐
USB ─────┤  Monitor (serial display)                    │
         │                        ┌───────────────────┐ │
         │  STM32U585 / Zephyr    │  Qualcomm Linux   │ │
         │  ─────────────────     │  ───────────────  │ │
         │  Modulino Thermo       │  demo_app (C++)   │ │
         │  (HS3003 via Wire1)    │  • training       │ │
         │  Bridge.call()  ──────►│  • TFLite infer   │ │
         │  (sensor data,         │  • patch apply    │ │
         │   commands)    ◄──────│  • anomaly detect │ │
         └──────────────────────────────────────────────┘
                                        ▲
                              ADB push (patch.tmd)
```

The STM32 sketch sends a temperature float every 500 ms via
`Bridge.call("demo/tick", "<temp>")` and forwards Monitor keystrokes
via `Bridge.call("demo/cmd", "<key>")`.  The Linux app returns display
strings that the sketch prints to Monitor.

Patch delivery uses ADB rather than the serial link, so there is no
size limit from the STM32 RAM.

---

## Hardware

| Item | Notes |
|------|-------|
| **Arduino UNO Q** | STM32U585 + Qualcomm aarch64 Linux co-processor |
| **Arduino Modulino Thermo** | HS3003 temperature sensor, I2C address `0x44` on Wire1 (Qwiic) |
| **Qwiic cable** | Connects sensor to the UNO Q Qwiic port |
| **USB-C cable** | Powers the board and carries the serial link |

---

## Software requirements

### PC side

| Tool | Install |
|------|---------|
| **arduino-cli** | [arduino.cc](https://arduino.github.io/arduino-cli/latest/installation/) |
| **arduino:zephyr** platform | `arduino-cli core install arduino:zephyr` |
| **Python 3.9+** | [python.org](https://www.python.org) |
| **TensorFlow 2.x** | `pip install tensorflow` |
| **NumPy** | `pip install numpy` |

### Board Linux side

Deployed automatically by the deploy script (`tflite/deploy_service.sh`
or `edgeimpulse/deploy_service.sh`):

| Component | Purpose |
|-----------|---------|
| **demo_app** or **demo_app_ei** | Training, inference, patch apply |
| **libtensorflowlite_c.so** | TFLite C API inference (TFLite variant only) |
| **Edge Impulse SDK** | TFLite Micro + DSP + classifier (EI variant only) |
| **TinyMLDelta runtime** | `tmd_apply_patch_from_memory()` |

---

## Quick start

### Option A — TFLite C API variant (simpler)

```bash
# 1. One-time setup: install deps, deploy demo_app, flash sketch
./setup.sh

# 2. Run the full demo (training -> model -> patch -> inference)
python3 run_demo.py
```

### Option B — Edge Impulse SDK variant

```bash
# 1. Clone the EI SDK fork with external model loading support
git clone -b feature/external-model-loading \
  https://github.com/felixgalindo/edge-impulse-sdk-pack.git
export EI_SDK_LOCAL=$(pwd)/edge-impulse-sdk-pack

# 2. Deploy to board
./edgeimpulse/deploy_service.sh

# 3. Flash the sketch (same sketch for both variants)
arduino-cli compile --fqbn arduino:zephyr:unoq arduino/UnoQ_TinyMLDeltaDemo
arduino-cli upload  --fqbn arduino:zephyr:unoq --port /dev/cu.usbmodem* \
    arduino/UnoQ_TinyMLDeltaDemo

# 4. Run the demo (training -> model -> patch -> inference)
python3 run_demo.py
```

---

## Manual step-by-step

### 1. Deploy the Linux app

**TFLite variant:**
```bash
./tflite/deploy_service.sh
```

**Edge Impulse variant:**
```bash
# Clone the fork if you haven't already (see "Edge Impulse SDK variant" below)
EI_SDK_LOCAL=/path/to/edge-impulse-sdk-pack ./edgeimpulse/deploy_service.sh
```

This pushes sources, the TinyMLDelta runtime, and the inference engine
to the board, builds on-board, and starts the service.

### 2. Flash the sketch

```bash
arduino-cli compile --fqbn arduino:zephyr:unoq arduino/UnoQ_TinyMLDeltaDemo
arduino-cli upload  --fqbn arduino:zephyr:unoq --port /dev/cu.usbmodem* \
    arduino/UnoQ_TinyMLDeltaDemo
```

### 3. Start training

Open the Arduino IDE Serial Monitor and send `t`, or use the FIFO:

```bash
ADB=~/Library/Arduino15/packages/arduino/tools/adb/32.0.0/adb
$ADB shell "echo t > /home/arduino/tinymldelta/cmd.fifo"
```

Wait ~100 seconds for 200 samples, then pull the CSV:

```bash
$ADB pull /home/arduino/tinymldelta/training_data.csv .
```

### 4. Generate model and patch

```bash
python3 make_model.py training_data.csv
```

Output files:

| File | Description |
|------|-------------|
| `base.tflite` | Autoencoder trained on your data |
| `target.tflite` | Fine-tuned variant (the update target) |
| `model_config.json` | Z-score normalization parameters (mean, std) |
| `patch.tmd` | Binary TinyMLDelta patch (base -> target) |

### 5. Push base model, config, and patch, then apply

```bash
$ADB push base.tflite       /home/arduino/tinymldelta/model.tflite
$ADB push model_config.json /home/arduino/tinymldelta/model_config.json
$ADB push patch.tmd         /home/arduino/tinymldelta/pending_patch.tmd
$ADB shell "echo u > /home/arduino/tinymldelta/cmd.fifo"
```

### 6. Start inference

```bash
$ADB shell "echo i > /home/arduino/tinymldelta/cmd.fifo"
# Watch output:
./tflite/deploy_service.sh --logs       # TFLite variant
./edgeimpulse/deploy_service.sh --logs  # EI variant
```

Output:
```
TEMP 24.52 C  score=0.0000
TEMP 24.53 C  score=0.0000
TEMP 32.50 C  score=0.0600  *** ANOMALY ***
```

Warm or cool the sensor to trigger anomalies.

---

## Model architecture

The autoencoder detects anomalies by learning to reconstruct normal
temperature patterns. Anomalous readings produce high reconstruction error.

```
Input (4 z-score normalized floats)
  -> Dense(8, relu)
  -> Dense(4, relu)     [bottleneck]
  -> Dense(4, linear)   [reconstruction]

Anomaly score = MSE(input, reconstruction)
Anomaly if score > 0.04
```

**Z-score normalization**: `(temp - mean) / std` where `mean` and `std`
are computed from training data (std floored at 3.0 C to tolerate normal
room temperature drift).

---

## Demo results

```
Base model:     2,340 bytes  (train MSE: 0.00216)
Target model:   2,380 bytes  (train MSE: 0.00142)
Patch:          1,691 bytes  (9 chunks, CRC32)
Patch apply:    < 1 ms  (on board)
```

The patch is **72% of the full model** — with larger real-world models
(20-200+ KB), patches are typically **< 5%** of the model size.

---

## Commands

| Key | Action |
|-----|--------|
| `t` | Start Training mode (collect 200 samples) |
| `i` | Toggle Inference mode |
| `u` | Apply pending `.tmd` patch |
| `s` | Print status |
| `?` | Print help menu |

Commands can be sent via:
- **Arduino IDE Serial Monitor** (type a key and press Enter)
- **FIFO**: `adb shell "echo t > /home/arduino/tinymldelta/cmd.fifo"`
- **`run_demo.py`** (automated end-to-end)

---

## How TinyMLDelta works in this demo

```
PC                          UNO Q Linux            STM32 (sensor proxy)
────────────────────────────────────────────────────────────────────────
python3 make_model.py
  |- base.tflite
  |- model_config.json
  |- patch.tmd
       |
adb push ───────────────► /home/arduino/tinymldelta/
                             model.tflite
                             model_config.json
                             pending_patch.tmd
                                    |
                    cmd: 'u' ──────►  demo_app
                                        apply_patch()
                                          |- read base model
                                          |- tmd_apply_patch_from_memory()
                                          |- write updated model.tflite
                                        reload interpreter
                                    |
                    cmd: 'i' ──────►  demo_app
                                        inference_tick(temp_c)
                                          |- z-score normalize
                                          |- TFLite invoke (or EI run_classifier)
                                          |- MSE > threshold? ANOMALY
```

---

## Edge Impulse SDK variant

The `edgeimpulse/` directory provides an alternative inference engine
using the Edge Impulse SDK with **external model loading**.

Key differences from the TFLite variant:

- Uses EI `run_classifier()` instead of the raw TFLite C API
- EI SDK bundles TFLite Micro — no separate `libtensorflowlite_c.so`
- Requires a patched EI SDK with `EI_CLASSIFIER_EXTERNAL_MODEL_LOADING`
- Model is loaded at runtime via `tmd_ei_model_loader()` callback
- Includes EI-exported `model-parameters/` and `tflite-model/` directories

### Setting up the EI SDK

Clone the fork with external model loading support:

```bash
git clone -b feature/external-model-loading \
  https://github.com/felixgalindo/edge-impulse-sdk-pack.git
export EI_SDK_LOCAL=$(pwd)/edge-impulse-sdk-pack
```

The fork adds `EI_CLASSIFIER_EXTERNAL_MODEL_LOADING` to the official
Edge Impulse SDK, enabling `run_classifier()` to load models from
external storage at runtime (e.g., TinyMLDelta-managed flash slots)
instead of using a compiled-in const array.

### Build details

The EI variant compiles ~165 source files (TFLite Micro kernels, DSP,
porting layer) on the board. First build takes ~7 minutes with `-j4`.
The resulting binary is ~1 MB.

Key compile flags:
- `-DEI_CLASSIFIER_EXTERNAL_MODEL_LOADING` — enables runtime model loading
- `-DEI_PORTING_POSIX=1` — POSIX porting layer (Linux)
- `-DEIDSP_USE_CMSIS_DSP=0` — disables CMSIS-DSP (not needed for this demo)

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No ticks in log | Reset board; check Modulino Thermo Qwiic connection |
| `Model not found` | Push `base.tflite` as `model.tflite` via ADB |
| `No pending patch found` | `adb push patch.tmd /home/arduino/tinymldelta/pending_patch.tmd` |
| Normalization warning | Push `model_config.json` via ADB (generated by `make_model.py`) |
| App not running | `./tflite/deploy_service.sh --restart` or `./edgeimpulse/deploy_service.sh --restart` |
| ADB not found | Check path in deploy script; install via arduino-cli |
| `pip install tensorflow` fails | Use Python 3.9-3.12 |
| EI build takes too long | Normal — ~7 min on board for 165 source files |

---

## Files

```
UnoQ_TinyMLDeltaDemo/
├── arduino/
│   └── UnoQ_TinyMLDeltaDemo/
│       └── UnoQ_TinyMLDeltaDemo.ino   STM32 sensor proxy sketch (HS3003 via Wire1)
├── common/                            Shared headers (both demo variants)
│   ├── msgpack.h                      MsgPack encoder/decoder (header-only)
│   ├── router_client.h                Arduino-router RPC client (header-only)
│   └── tmd_port_memory.h              In-memory TinyMLDelta flash port
├── tflite/                            TFLite C API variant
│   ├── demo_app.cpp                   Main demo app (TFLite C API inference)
│   ├── Makefile                       Build config for demo_app
│   └── deploy_service.sh              Push + build + manage demo_app via ADB
├── edgeimpulse/                       Edge Impulse SDK variant
│   ├── demo_app_ei.cpp                Demo app (EI run_classifier inference)
│   ├── Makefile                       Build config for demo_app_ei
│   ├── deploy_service.sh              Push + build + manage demo_app_ei via ADB
│   ├── model-parameters/              EI-exported model metadata
│   │   ├── model_metadata.h           Classifier config (sample count, frequency)
│   │   └── model_variables.h          Model variable definitions
│   └── tflite-model/                  EI-compiled TFLite model
│       ├── tflite_learn_*_compiled.cpp  Compiled model as C array
│       ├── tflite_learn_*_compiled.h    Header for compiled model
│       └── trained_model_ops_define.h   Op resolver definitions
├── make_model.py                      Train autoencoder, generate patch.tmd
├── run_demo.py                        Automated end-to-end demo runner
├── setup.sh                           One-time dependency install + deploy + flash
└── README.md                          This file
```
