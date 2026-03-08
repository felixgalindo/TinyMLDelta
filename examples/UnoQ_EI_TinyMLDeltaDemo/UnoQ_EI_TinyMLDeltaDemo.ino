/*
 * TinyMLDelta — Qwiic Thermo Anomaly Demo (Edge Impulse Edition)
 * ---------------------------------------------------------------
 * Arduino UNO R4 ("UNO Q") + SparkFun Qwiic MCP9600 thermocouple sensor
 *
 * ** STUB — Edge Impulse integration coming soon **
 *
 * This sketch mirrors the structure of arduino_uno_q_tflm/ but replaces
 * the hand-rolled TFLite Micro setup with an Edge Impulse exported library.
 *
 * When the Edge Impulse library is available, replace the TODO sections
 * below with the generated ei_* calls. The TinyMLDelta model-update flow
 * (Training → Update → Inference) and sensor integration are identical to
 * the TFLM example.
 *
 * Three operating modes (same as TFLM example):
 *
 *   TRAINING  — Collect normal temperature samples; output CSV over Serial.
 *   INFERENCE — Poll sensor, run anomaly classifier, print flags.
 *   UPDATE    — Receive TinyMLDelta patch, apply, reload EI model.
 *
 * Serial commands (115200 baud):
 *   t / T  — Training mode
 *   i / I  — Inference mode  (toggle)
 *   u / U  — Update mode
 *   s / S  — Status
 *   ?      — Menu
 *
 * Edge Impulse workflow (when EI library is ready):
 *   1. Collect training CSV in Training mode (same as TFLM example).
 *   2. Upload the CSV to an Edge Impulse project.
 *   3. Train an anomaly detection block (K-means or GMM) in EI Studio.
 *   4. Export as "Arduino library", install in IDE.
 *   5. Uncomment the EI includes below and fill in the TODO blocks.
 *   6. Use make_model_ei.py (TBD) to generate a TinyMLDelta patch from
 *      the EI exported model bytes when you retrain.
 *
 * Required libraries:
 *   - SparkFun MCP9600 Thermocouple Amplifier
 *   - <your-project-name>_inferencing  (exported from Edge Impulse)
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_MCP9600.h>

/*
 * TODO: Uncomment and replace with your Edge Impulse exported library header.
 *
 * #include <your_project_name_inferencing.h>
 */

#include "tinymldelta.h"
#include "tinymldelta_ports.h"
#include "tinymldelta_config.h"
#include "flash_layout_uno_q.h"

/* Declared in tinymldelta_ports_arduino.cpp */
extern void           tmd_arduino_init_slots(const uint8_t* base, size_t len);
extern const uint8_t* tmd_arduino_active_slot_ptr(void);

/* =========================================================================
 * Configuration (keep in sync with the EI project settings)
 * ====================================================================== */

static const uint8_t  kSensorAddr       = 0x60;
static const uint32_t kSensorIntervalMs = 500;
static const float    kTempMin          = -10.0f;
static const float    kTempMax          =  85.0f;
static const size_t   kPatchBufSize     = 4096;
static const int      kMaxTrainSamples  = 200;

/* TODO: Set to EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / EI_CLASSIFIER_RAW_SAMPLE_COUNT
 * once you have the EI library included.                                   */
static const int      kWindowSize       = 4;

/* =========================================================================
 * Mode state machine
 * ====================================================================== */

typedef enum { MODE_IDLE, MODE_TRAINING, MODE_INFERENCE, MODE_UPDATE } AppMode;
static AppMode g_mode = MODE_IDLE;

/* =========================================================================
 * Global state
 * ====================================================================== */

static MCP9600 g_sensor;

/* Z-score baseline (Welford online, used as fallback and in training display) */
static float    g_z_mean = 0.0f;
static float    g_z_M2   = 0.0f;
static uint32_t g_z_n    = 0;

/* Training buffer */
static float g_train_buf[kMaxTrainSamples];
static int   g_train_n   = 0;

/* Last readings */
static float    g_last_temp_c  = 0.0f;
static float    g_last_score   = 0.0f;
static bool     g_last_anomaly = false;
static uint32_t g_readings     = 0;

/* Patch buffer */
static uint8_t g_patch_buf[kPatchBufSize];

/* =========================================================================
 * Temperature helpers
 * ====================================================================== */

static float normalize_temp(float t_c) {
    float v = (t_c - kTempMin) / (kTempMax - kTempMin);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

/* =========================================================================
 * Z-score helpers
 * ====================================================================== */

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
 * Edge Impulse inference (TODO)
 * ====================================================================== */

/*
 * ei_run_anomaly(temp_c, out_score)
 * ----------------------------------
 * TODO: Replace the stub below with actual Edge Impulse inference.
 *
 * Typical EI pattern:
 *
 *   float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
 *   // fill features[] with your normalized window samples
 *
 *   signal_t signal;
 *   numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
 *
 *   ei_impulse_result_t result;
 *   EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
 *   if (err != EI_IMPULSE_OK) return false;
 *
 *   // For anomaly detection block:
 *   *out_score = result.anomaly;
 *   return true;
 */
static bool ei_run_anomaly(float /*temp_c*/, float* out_score) {
    /* Stub — Z-score fallback until EI library is integrated */
    *out_score = 0.0f;
    return false;   /* returning false triggers Z-score fallback in inference_tick */
}

/* =========================================================================
 * MODE: TRAINING
 * ====================================================================== */

static void training_start(void) {
    g_train_n = 0;
    zscore_reset();
    Serial.println(F("[TRAIN] Training mode. Collecting normal temperature samples."));
    Serial.print(F("[TRAIN] Will collect "));
    Serial.print(kMaxTrainSamples);
    Serial.println(F(" samples, then print CSV."));
}

static void training_tick(float temp_c) {
    if (g_train_n >= kMaxTrainSamples) return;
    g_train_buf[g_train_n++] = temp_c;
    zscore_train(temp_c);
    if (g_train_n % 10 == 0) {
        Serial.print(F("[TRAIN] ")); Serial.print(g_train_n);
        Serial.print(F("/")); Serial.print(kMaxTrainSamples);
        Serial.print(F("  ")); Serial.print(temp_c, 2); Serial.println(F(" C"));
    }
    if (g_train_n >= kMaxTrainSamples) {
        Serial.println(F("[TRAIN] Done. --- CSV BEGIN ---"));
        Serial.println(F("temp_c"));
        for (int i = 0; i < g_train_n; ++i) Serial.println(g_train_buf[i], 4);
        Serial.println(F("[TRAIN] --- CSV END ---"));
        Serial.println(F("[TRAIN] Upload CSV to Edge Impulse, retrain, export library."));
        Serial.println(F("[TRAIN] Then run make_model_ei.py to generate patch.tmd."));
        g_mode = MODE_IDLE;
    }
}

/* =========================================================================
 * MODE: INFERENCE
 * ====================================================================== */

static void inference_tick(float temp_c) {
    g_last_temp_c = temp_c;
    ++g_readings;

    float score   = 0.0f;
    bool  anomaly = false;
    bool  ei_ok   = ei_run_anomaly(temp_c, &score);

    if (!ei_ok) {
        /* Fallback to Z-score */
        score   = zscore_score(temp_c);
        anomaly = (g_z_n > 10) && (score > 3.0f);
    } else {
        anomaly = (score > 0.5f);   /* EI anomaly score threshold — tune per model */
    }

    g_last_score   = score;
    g_last_anomaly = anomaly;

    Serial.print(F("TEMP ")); Serial.print(temp_c, 2);
    Serial.print(F(" C  score=")); Serial.print(score, 4);
    if (!ei_ok) Serial.print(F(" [Z]"));
    if (anomaly) Serial.print(F("  *** ANOMALY ***"));
    Serial.println();
}

/* =========================================================================
 * MODE: UPDATE
 * ====================================================================== */

static size_t receive_patch(uint8_t* buf, size_t cap) {
    Serial.println(F("[UPDATE] Waiting for 4-byte LE length prefix (10 s)..."));
    uint32_t deadline = millis() + 10000UL;
    while (!Serial.available()) {
        if (millis() > deadline) { Serial.println(F("[UPDATE] Timeout.")); return 0; }
    }
    uint8_t lbuf[4] = {0}; size_t got = 0;
    deadline = millis() + 3000UL;
    while (got < 4 && millis() < deadline)
        if (Serial.available()) lbuf[got++] = (uint8_t)Serial.read();
    if (got < 4) { Serial.println(F("[UPDATE] Bad length.")); return 0; }
    uint32_t plen = (uint32_t)lbuf[0] | ((uint32_t)lbuf[1]<<8) | ((uint32_t)lbuf[2]<<16) | ((uint32_t)lbuf[3]<<24);
    if (!plen || plen > cap) { Serial.print(F("[UPDATE] Length out of range: ")); Serial.println(plen); return 0; }
    Serial.print(F("[UPDATE] Receiving ")); Serial.print(plen); Serial.println(F(" bytes..."));
    size_t n = 0; deadline = millis() + 10000UL;
    while (n < plen && millis() < deadline)
        if (Serial.available()) { buf[n++] = (uint8_t)Serial.read(); deadline = millis() + 2000UL; }
    if (n != plen) { Serial.println(F("[UPDATE] Short read.")); return 0; }
    return n;
}

static void update_start(void) {
    Serial.println(F("[UPDATE] Run:  python3 send_patch.py <port> patch.tmd"));
    size_t plen = receive_patch(g_patch_buf, kPatchBufSize);
    if (!plen) { g_mode = MODE_IDLE; return; }

    Serial.print(F("[UPDATE] Applying ")); Serial.print(plen); Serial.println(F(" B..."));
    tmd_status_t st = tmd_apply_patch_from_memory(g_patch_buf, plen);
    if (st != TMD_STATUS_OK) {
        Serial.print(F("[UPDATE] FAILED status=")); Serial.println((int)st);
        g_mode = MODE_IDLE; return;
    }

    /* TODO: Reload the EI model from the active slot.
     *
     * Edge Impulse models are typically linked into firmware at build time,
     * so a live swap requires either:
     *   a) Patching the model weights blob inside the EI library's data segment
     *      (advanced — requires understanding EI's internal layout), or
     *   b) Implementing a custom tflite backend in EI that loads from RAM.
     *
     * For now, print a notice. This placeholder will be replaced when the
     * EI integration is implemented.
     */
    Serial.println(F("[UPDATE] Patch applied. EI model reload: TODO."));
    Serial.println(F("[UPDATE] Reset device to activate updated model."));

    g_mode = MODE_IDLE;
}

/* =========================================================================
 * Status + menu
 * ====================================================================== */

static void print_menu(void) {
    Serial.println(F("Commands:  t=train  i=infer  u=update  s=status  ?=help"));
}

static void print_status(void) {
    const tmd_ports_t*  P = tmd_ports();
    const tmd_layout_t* L = tmd_layout();
    uint8_t si = P->get_active_slot();
    const tmd_slot_t* slot = (si == 0) ? &L->slotA : &L->slotB;
    Serial.println(F("--- STATUS (Edge Impulse edition) --------"));
    Serial.print(F("Active slot : ")); Serial.println(si);
    Serial.print(F("Slot addr   : 0x")); Serial.println(slot->addr, HEX);
    Serial.print(F("Last temp   : ")); Serial.print(g_last_temp_c, 2); Serial.println(F(" C"));
    Serial.print(F("Last score  : ")); Serial.println(g_last_score, 4);
    Serial.print(F("Anomaly     : ")); Serial.println(g_last_anomaly ? F("YES ***") : F("no"));
    Serial.print(F("Readings    : ")); Serial.println(g_readings);
    Serial.print(F("Z baseline  : n=")); Serial.print(g_z_n);
    Serial.print(F("  mean=")); Serial.print(g_z_mean, 2); Serial.println(F(" C"));
    Serial.println(F("-----------------------------------------"));
}

static void handle_command(char c) {
    switch (c) {
        case 't': case 'T': g_mode = MODE_TRAINING;  training_start(); break;
        case 'i': case 'I':
            if (g_mode == MODE_INFERENCE) { g_mode = MODE_IDLE; Serial.println(F("[INFER] Stopped.")); }
            else { g_mode = MODE_INFERENCE; Serial.println(F("[INFER] Started.")); }
            break;
        case 'u': case 'U': g_mode = MODE_UPDATE; update_start(); break;
        case 's': case 'S': print_status(); break;
        case '?': print_menu(); break;
        default: break;
    }
}

/* =========================================================================
 * Arduino entry points
 * ====================================================================== */

void setup(void) {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println();
    Serial.println(F("================================================"));
    Serial.println(F(" TinyMLDelta Qwiic Thermo Demo — Edge Impulse"));
    Serial.println(F(" Arduino UNO R4 | MCP9600 | EI (stub)"));
    Serial.println(F("================================================"));
    Serial.println(F("NOTE: Edge Impulse integration is a stub."));
    Serial.println(F("      Z-score fallback is active until EI is wired in."));
    print_menu();
    Serial.println();

    Wire.begin();
    if (!g_sensor.begin(kSensorAddr)) {
        Serial.println(F("[SENSOR] MCP9600 not found at 0x60! Check Qwiic cable."));
    } else {
        g_sensor.setAmbientResolution(RES_ZERO_POINT_0625);
        g_sensor.setThermocoupleResolution(RES_14_BIT);
        Serial.println(F("[SENSOR] MCP9600 ready."));
    }

    /* Init TinyMLDelta RAM slots (no base model yet for EI variant). */
    tmd_arduino_init_slots(nullptr, 0);

    Serial.println(F("Ready. Send 't' to collect training data."));
    Serial.println();
}

void loop(void) {
    static uint32_t last_tick = 0;
    uint32_t now = millis();

    if (now - last_tick >= kSensorIntervalMs) {
        last_tick = now;
        if (g_mode == MODE_TRAINING || g_mode == MODE_INFERENCE) {
            if (!g_sensor.isConnected()) {
                Serial.println(F("[SENSOR] Not connected."));
            } else {
                float t = g_sensor.getThermocoupleTemp();
                if (g_mode == MODE_TRAINING)  training_tick(t);
                if (g_mode == MODE_INFERENCE) inference_tick(t);
            }
        }
    }

    if (Serial.available()) {
        int c = Serial.read();
        if (c > 0) handle_command((char)c);
    }
}
