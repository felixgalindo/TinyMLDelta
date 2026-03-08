/*
 * TinyMLDelta — Qwiic Thermo Anomaly Demo
 * ----------------------------------------
 * Arduino UNO Q + SparkFun Qwiic MCP9600 thermocouple sensor
 *
 * Architecture note (UNO Q):
 *   The UNO Q runs Zephyr RTOS on an STM32U5 co-processor paired with a
 *   Linux host processor. Console I/O uses Monitor (routed via Bridge RPC),
 *   NOT the standard Serial object. Wire (I2C / Qwiic) works normally.
 *
 * Three operating modes:
 *
 *   TRAINING  — Collects temperature samples from normal operation.
 *               Outputs CSV over Monitor for PC-side model training.
 *               Updates the on-device Z-score baseline.
 *
 *   INFERENCE — Polls sensor every 500 ms, computes anomaly score,
 *               prints readings + "*** ANOMALY ***" flags.
 *               Uses TFLite autoencoder when a valid model is loaded,
 *               falls back to Z-score otherwise.
 *
 *   UPDATE    — Receives a TinyMLDelta patch (.tmd) over Monitor,
 *               applies it to the inactive RAM slot, flips active slot,
 *               then reloads the model.
 *
 * Monitor commands (open via Arduino IDE Serial Monitor or send_patch.py):
 *   t / T  — Training mode
 *   i / I  — Inference mode (toggle)
 *   u / U  — Update mode (send patch with send_patch.py)
 *   s / S  — Status
 *   ?      — Menu
 *
 * Required libraries (Arduino Library Manager):
 *   - Arduino_RouterBridge  (built-in to arduino:zephyr platform)
 *   - SparkFun MCP9600 Thermocouple Library
 *   - Arduino_TensorFlowLite  (optional; set TMD_HAS_TFLM=0 to disable)
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_RouterBridge.h>
#include <SparkFun_MCP9600.h>

/* Set to 0 to build without TFLite (Z-score only mode).
 * Can also be overridden at compile time: -DTMD_HAS_TFLM=0             */
#ifndef TMD_HAS_TFLM
#define TMD_HAS_TFLM 0   /* disabled until TFLite confirmed on UNO Q  */
#endif

#if TMD_HAS_TFLM
#include <Chirale_TensorFlowLite.h>   /* Chirale_TensorFLowLite library */
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>
#endif

#include "tinymldelta.h"
#include "tinymldelta_ports.h"
#include "tinymldelta_config.h"
#include "flash_layout_uno_q.h"
#include "model.h"    /* kBaseModelData[], kBaseModelSize — run make_model.py */

extern void           tmd_arduino_init_slots(const uint8_t* base, size_t len);
extern const uint8_t* tmd_arduino_active_slot_ptr(void);

/* =========================================================================
 * Configuration
 * ====================================================================== */

static const uint8_t  kSensorAddr          = 0x60;
static const uint32_t kSensorIntervalMs    = 500;
static const int      kWindowSize          = 4;
static const float    kAnomalyThresholdMSE = 0.04f;
static const float    kAnomalyThresholdZ   = 3.0f;
static const float    kTempMin             = -10.0f;
static const float    kTempMax             =  85.0f;
static const size_t   kPatchBufSize        = 4096;
static const int      kMaxTrainSamples     = 200;
static const int      kTensorArenaSize     = 8 * 1024;

/* =========================================================================
 * Mode state machine
 * ====================================================================== */

typedef enum { MODE_IDLE, MODE_TRAINING, MODE_INFERENCE, MODE_UPDATE } AppMode;
static AppMode g_mode = MODE_IDLE;

/* =========================================================================
 * Global state
 * ====================================================================== */

static MCP9600 g_sensor;
static bool    g_sensor_ok = false;

static float   g_window[kWindowSize];
static int     g_window_count = 0;

static float    g_z_mean = 0.0f;
static float    g_z_M2   = 0.0f;
static uint32_t g_z_n    = 0;

static float   g_train_buf[kMaxTrainSamples];
static int     g_train_n   = 0;

static float    g_last_temp_c  = 0.0f;
static float    g_last_score   = 0.0f;
static bool     g_last_anomaly = false;
static uint32_t g_readings     = 0;

static uint8_t g_patch_buf[kPatchBufSize];
static bool    g_model_ok = false;

#if TMD_HAS_TFLM
static alignas(16) uint8_t g_tensor_arena[kTensorArenaSize];
static alignas(tflite::MicroInterpreter)
       uint8_t g_interp_buf[sizeof(tflite::MicroInterpreter)];
static tflite::MicroInterpreter*     g_interpreter  = nullptr;
static tflite::MicroMutableOpResolver<2> g_resolver;
static bool g_resolver_ready = false;
#endif

/* =========================================================================
 * Helpers
 * ====================================================================== */

static float normalize_temp(float t_c) {
    float v = (t_c - kTempMin) / (kTempMax - kTempMin);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static void window_push(float norm) {
    for (int i = 0; i < kWindowSize - 1; ++i) g_window[i] = g_window[i + 1];
    g_window[kWindowSize - 1] = norm;
    if (g_window_count < kWindowSize) ++g_window_count;
}

static void zscore_reset(void) { g_z_mean = 0; g_z_M2 = 0; g_z_n = 0; }

static void zscore_train(float t) {
    ++g_z_n;
    float d  = t - g_z_mean;
    g_z_mean += d / (float)g_z_n;
    g_z_M2   += d * (t - g_z_mean);
}

static float zscore_score(float t) {
    if (g_z_n < 2) return 0.0f;
    float std = sqrtf(g_z_M2 / (float)(g_z_n - 1));
    if (std < 0.01f) return 0.0f;
    return fabsf(t - g_z_mean) / std;
}

/* =========================================================================
 * TFLM model (when enabled)
 * ====================================================================== */

#if TMD_HAS_TFLM
static bool init_model(const uint8_t* bytes, size_t size) {
    if (!bytes || size < 8) return false;
    const tflite::Model* model = tflite::GetModel(bytes);
    if (!model || model->version() != TFLITE_SCHEMA_VERSION) {
        Monitor.println(F("[TFLM] Invalid model."));
        return false;
    }
    if (!g_resolver_ready) {
        g_resolver.AddFullyConnected();
        g_resolver.AddRelu();
        g_resolver_ready = true;
    }
    if (g_interpreter) { g_interpreter->~MicroInterpreter(); g_interpreter = nullptr; }
    g_interpreter = new (g_interp_buf) tflite::MicroInterpreter(
        model, g_resolver, g_tensor_arena, kTensorArenaSize);
    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        Monitor.println(F("[TFLM] AllocateTensors failed."));
        g_interpreter = nullptr; return false;
    }
    Monitor.print(F("[TFLM] OK. Arena: "));
    Monitor.print(g_interpreter->arena_used_bytes());
    Monitor.println(F(" B"));
    return true;
}

static bool run_inference(const float* win, float* out_score) {
    if (!g_interpreter) return false;
    TfLiteTensor* inp = g_interpreter->input(0);
    for (int i = 0; i < kWindowSize; ++i) inp->data.f[i] = win[i];
    if (g_interpreter->Invoke() != kTfLiteOk) return false;
    TfLiteTensor* out = g_interpreter->output(0);
    float mse = 0;
    for (int i = 0; i < kWindowSize; ++i) {
        float d = win[i] - out->data.f[i]; mse += d * d;
    }
    *out_score = mse / kWindowSize;
    return true;
}
#endif

/* =========================================================================
 * MODE: TRAINING
 * ====================================================================== */

static void training_start(void) {
    g_train_n = 0;
    zscore_reset();
    memset(g_window, 0, sizeof(g_window));
    g_window_count = 0;
    Monitor.println(F("[TRAIN] Collecting normal temperature samples."));
    Monitor.print(F("[TRAIN] Target: "));
    Monitor.print(kMaxTrainSamples);
    Monitor.println(F(" samples. Keep sensor at normal operating temperature."));
}

static void training_tick(float temp_c) {
    if (g_train_n >= kMaxTrainSamples) return;
    g_train_buf[g_train_n++] = temp_c;
    zscore_train(temp_c);
    if (g_train_n % 10 == 0) {
        Monitor.print(F("[TRAIN] ")); Monitor.print(g_train_n);
        Monitor.print(F("/")); Monitor.print(kMaxTrainSamples);
        Monitor.print(F("  ")); Monitor.print(temp_c, 2); Monitor.println(F(" C"));
    }
    if (g_train_n >= kMaxTrainSamples) {
        Monitor.println(F("[TRAIN] Done. --- CSV BEGIN ---"));
        Monitor.println(F("temp_c"));
        for (int i = 0; i < g_train_n; ++i) Monitor.println(g_train_buf[i], 4);
        Monitor.println(F("[TRAIN] --- CSV END ---"));
        float var = (g_z_n > 1) ? g_z_M2 / (float)(g_z_n - 1) : 0.0f;
        Monitor.print(F("[TRAIN] Baseline: mean="));
        Monitor.print(g_z_mean, 2); Monitor.print(F(" std="));
        Monitor.print(sqrtf(var), 2); Monitor.println(F(" C"));
        Monitor.println(F("[TRAIN] Run: python3 make_model.py --csv training_data.csv"));
        g_mode = MODE_IDLE;
    }
}

/* =========================================================================
 * MODE: INFERENCE
 * ====================================================================== */

static void inference_start(void) {
    memset(g_window, 0, sizeof(g_window));
    g_window_count = 0;
    Monitor.println(F("[INFER] Started."));
#if TMD_HAS_TFLM
    Monitor.println(g_model_ok ? F("[INFER] Using TFLM autoencoder.")
                              : F("[INFER] Using Z-score fallback."));
#else
    Monitor.println(F("[INFER] Z-score mode (TFLM not compiled in)."));
#endif
    if (g_z_n < 10) Monitor.println(F("[INFER] Tip: run Training mode first."));
}

static void inference_tick(float temp_c) {
    g_last_temp_c = temp_c;
    ++g_readings;
    float norm = normalize_temp(temp_c);
    window_push(norm);

    float score = 0.0f; bool anomaly = false;

#if TMD_HAS_TFLM
    if (g_model_ok && g_window_count >= kWindowSize) {
        if (run_inference(g_window, &score)) anomaly = (score > kAnomalyThresholdMSE);
    } else {
        score = zscore_score(temp_c);
        anomaly = (g_z_n > 10) && (score > kAnomalyThresholdZ);
    }
#else
    score = zscore_score(temp_c);
    anomaly = (g_z_n > 10) && (score > kAnomalyThresholdZ);
#endif

    g_last_score = score; g_last_anomaly = anomaly;

    Monitor.print(F("TEMP ")); Monitor.print(temp_c, 2);
    Monitor.print(F(" C  score=")); Monitor.print(score, 4);
    if (anomaly) Monitor.print(F("  *** ANOMALY ***"));
    Monitor.println();
}

/* =========================================================================
 * MODE: UPDATE
 * ====================================================================== */

static size_t receive_patch(uint8_t* buf, size_t cap) {
    Monitor.println(F("[UPDATE] Waiting for 4-byte LE length prefix (15 s)..."));

    uint32_t deadline = millis() + 15000UL;
    while (!Monitor.available()) {
        if (millis() > deadline) {
            Monitor.println(F("[UPDATE] Timeout."));
            return 0;
        }
        delay(10);
    }

    /* Read 4-byte little-endian length */
    uint8_t lbuf[4] = {0};
    size_t  got = 0;
    deadline = millis() + 3000UL;
    while (got < 4 && millis() < deadline) {
        if (Monitor.available()) lbuf[got++] = (uint8_t)Monitor.read();
    }
    if (got < 4) { Monitor.println(F("[UPDATE] Bad length.")); return 0; }

    uint32_t plen = (uint32_t)lbuf[0]
                  | ((uint32_t)lbuf[1] <<  8)
                  | ((uint32_t)lbuf[2] << 16)
                  | ((uint32_t)lbuf[3] << 24);

    if (!plen || plen > (uint32_t)cap) {
        Monitor.print(F("[UPDATE] Length out of range: ")); Monitor.println(plen);
        return 0;
    }

    Monitor.print(F("[UPDATE] Receiving ")); Monitor.print(plen); Monitor.println(F(" bytes..."));

    size_t n = 0;
    deadline = millis() + 15000UL;
    while (n < (size_t)plen && millis() < deadline) {
        if (Monitor.available()) {
            buf[n++] = (uint8_t)Monitor.read();
            deadline  = millis() + 3000UL;
        } else {
            delay(1);
        }
    }

    if (n != (size_t)plen) {
        Monitor.print(F("[UPDATE] Short read: ")); Monitor.print(n);
        Monitor.print(F(" / ")); Monitor.println(plen);
        return 0;
    }
    return n;
}

static void update_start(void) {
    Monitor.println(F("[UPDATE] Ready. On your PC run:"));
    Monitor.println(F("[UPDATE]   python3 send_patch.py <port> patch.tmd"));

    size_t plen = receive_patch(g_patch_buf, kPatchBufSize);
    if (!plen) { g_mode = MODE_IDLE; return; }

    Monitor.print(F("[UPDATE] Applying ")); Monitor.print(plen); Monitor.println(F(" B..."));

    tmd_status_t st = tmd_apply_patch_from_memory(g_patch_buf, plen);
    if (st != TMD_STATUS_OK) {
        Monitor.print(F("[UPDATE] FAILED status=")); Monitor.println((int)st);
        g_mode = MODE_IDLE; return;
    }

    Monitor.println(F("[UPDATE] Patch applied. Reloading model..."));
    const uint8_t* new_model = tmd_arduino_active_slot_ptr();

#if TMD_HAS_TFLM
    g_model_ok = init_model(new_model, TMD_SLOT_BYTES);
    Monitor.println(g_model_ok ? F("[UPDATE] New model active.")
                              : F("[UPDATE] Reload failed — Z-score mode."));
#else
    (void)new_model;
    Monitor.println(F("[UPDATE] Done (Z-score mode — TFLM not compiled in)."));
#endif

    memset(g_window, 0, sizeof(g_window));
    g_window_count = 0;
    g_mode = MODE_IDLE;
    Monitor.println(F("Send 'i' to resume inference."));
}

/* =========================================================================
 * Status + menu
 * ====================================================================== */

static void print_menu(void) {
    Monitor.println(F("Commands:  t=train  i=infer  u=update  s=status  ?=help"));
}

static void print_status(void) {
    const tmd_ports_t*  P = tmd_ports();
    const tmd_layout_t* L = tmd_layout();
    uint8_t si = P->get_active_slot();
    const tmd_slot_t* slot = (si == 0) ? &L->slotA : &L->slotB;
    const char* mode_str =
        g_mode == MODE_TRAINING  ? "TRAINING"  :
        g_mode == MODE_INFERENCE ? "INFERENCE" :
        g_mode == MODE_UPDATE    ? "UPDATE"    : "IDLE";

    Monitor.println(F("--- STATUS ----------------------------------------"));
    Monitor.print(F("Mode        : ")); Monitor.println(mode_str);
    Monitor.print(F("Sensor      : ")); Monitor.println(g_sensor_ok ? F("OK") : F("NOT FOUND"));
    Monitor.print(F("Active slot : ")); Monitor.println(si);
    Monitor.print(F("Slot addr   : 0x")); Monitor.println(slot->addr, HEX);
    Monitor.print(F("Model       : "));
    Monitor.println(g_model_ok ? F("TFLM autoencoder") : F("Z-score fallback"));
    Monitor.print(F("Z baseline  : n=")); Monitor.print(g_z_n);
    Monitor.print(F("  mean=")); Monitor.print(g_z_mean, 2);
    float var = (g_z_n > 1) ? g_z_M2 / (float)(g_z_n - 1) : 0.0f;
    Monitor.print(F(" C  std=")); Monitor.print(sqrtf(var), 2); Monitor.println(F(" C"));
    Monitor.print(F("Readings    : ")); Monitor.println(g_readings);
    Monitor.print(F("Last temp   : ")); Monitor.print(g_last_temp_c, 2); Monitor.println(F(" C"));
    Monitor.print(F("Last score  : ")); Monitor.println(g_last_score, 4);
    Monitor.print(F("Anomaly     : ")); Monitor.println(g_last_anomaly ? F("YES ***") : F("no"));
    Monitor.println(F("---------------------------------------------------"));
}

static void handle_command(char c) {
    switch (c) {
        case 't': case 'T':
            g_mode = MODE_TRAINING; training_start(); break;
        case 'i': case 'I':
            if (g_mode == MODE_INFERENCE) {
                g_mode = MODE_IDLE;
                Monitor.print(F("[INFER] Stopped. Readings: "));
                Monitor.println(g_readings);
            } else {
                g_mode = MODE_INFERENCE; inference_start();
            }
            break;
        case 'u': case 'U':
            g_mode = MODE_UPDATE; update_start(); break;
        case 's': case 'S':
            print_status(); break;
        case '?':
            print_menu(); break;
        default: break;
    }
}

/* =========================================================================
 * Arduino entry points
 * ====================================================================== */

void setup(void) {
    Serial.begin(115200);   /* debug UART — not needed for UNO Q output  */

    Bridge.begin();
    Monitor.begin();

    Monitor.println();
    Monitor.println(F("================================================"));
    Monitor.println(F(" TinyMLDelta Qwiic Thermo Anomaly Demo"));
    Monitor.println(F(" Arduino UNO Q | MCP9600 | TinyMLDelta"));
    Monitor.println(F("================================================"));
    print_menu();
    Monitor.println();

    /* UNO Q Qwiic connector is on i2c4 (Wire1), not the header i2c2 (Wire) */
    Wire1.begin();
    g_sensor_ok = g_sensor.begin(kSensorAddr, Wire1);
    if (!g_sensor_ok) {
        Monitor.println(F("[SENSOR] MCP9600 not found at 0x60! Check Qwiic cable."));
    } else {
        g_sensor.setAmbientResolution(RES_ZERO_POINT_0625);
        g_sensor.setThermocoupleResolution(RES_14_BIT);
        Monitor.println(F("[SENSOR] MCP9600 ready."));
    }

    tmd_arduino_init_slots(kBaseModelData, kBaseModelSize);

#if TMD_HAS_TFLM
    g_model_ok = init_model(tmd_arduino_active_slot_ptr(), TMD_SLOT_BYTES);
    if (!g_model_ok) {
        Monitor.println(F("[TFLM] Base model invalid — Z-score fallback active."));
        Monitor.println(F("[TFLM] Run make_model.py to generate model.h."));
    }
#endif

    Monitor.println(F("Ready. Send 't' to collect training data first."));
    Monitor.println();
}

void loop(void) {
    static uint32_t last_tick = 0;
    uint32_t now = millis();

    if (now - last_tick >= kSensorIntervalMs) {
        last_tick = now;
        if (g_mode == MODE_TRAINING || g_mode == MODE_INFERENCE) {
            if (!g_sensor_ok) {
                Monitor.println(F("[SENSOR] Not connected."));
            } else {
                float t = g_sensor.getThermocoupleTemp();
                if (g_mode == MODE_TRAINING)  training_tick(t);
                if (g_mode == MODE_INFERENCE) inference_tick(t);
            }
        }
    }

    if (Monitor.available()) {
        int c = Monitor.read();
        if (c > 0) handle_command((char)c);
    }
}
