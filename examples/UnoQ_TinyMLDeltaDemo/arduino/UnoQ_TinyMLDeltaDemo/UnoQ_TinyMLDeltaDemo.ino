/*
 * TinyMLDelta — Temperature Anomaly Demo (Linux Edition)
 * --------------------------------------------------------
 * Arduino UNO Q + Arduino Modulino Thermo (HS3003 sensor, 0x44 via Qwiic)
 *
 * Architecture:
 *   This sketch is a thin STM32/Zephyr sensor proxy.  All application
 *   logic (training, inference, anomaly detection, model update) runs on
 *   the UNO Q Linux co-processor in demo_app (C++/TFLite).
 *
 *   Every 500 ms the sketch reads the HS3003 temperature and calls
 *   Bridge.call("demo/tick", "<temp_c>") — the Linux app returns a display
 *   line that is printed to Monitor.
 *
 *   Monitor keystrokes (t/i/u/s/?) are forwarded via
 *   Bridge.call("demo/cmd", "<key>") — the Linux app returns a response
 *   that is printed to Monitor.
 *
 * Deploy demo_app to the Linux side before flashing:
 *   ./linux/deploy_service.sh
 *
 * Required libraries (Arduino Library Manager):
 *   - Arduino_RouterBridge  (built-in to arduino:zephyr platform)
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_RouterBridge.h>

/* =========================================================================
 * Configuration
 * ====================================================================== */

static const uint32_t kSensorIntervalMs = 500;
static const uint8_t  kSensorAddr       = 0x44; /* Modulino Thermo / HS3003 */

/* Value sent to Linux when the sensor is not connected. */
static const float kBadTemp = -999.0f;

/* =========================================================================
 * Global state
 * ====================================================================== */

static TwoWire* g_wire      = nullptr;
static bool     g_sensor_ok = false;

/* =========================================================================
 * HS3003 raw I2C helpers
 *
 * The HS3003 measurement cycle:
 *   1. Measurement Request (MR): write 0 bytes to 0x44 (start measurement).
 *   2. Wait ≥ 33 ms (14-bit precision).
 *   3. Data Fetch (DF): read 4 bytes.
 *
 * Bytes: [status(2b)+hum_hi(6b)][hum_lo][temp_hi][temp_lo(6b)+_don'tcare(2b)]
 *   temp_raw = (byte[2] << 6) | (byte[3] >> 2)
 *   temp_c   = temp_raw / 16383.0 * 165.0 - 40.0
 * ====================================================================== */

/* Try a 0-byte write (MR) to check if the sensor ACKs the address.
 * Zephyr i2c_write with len=0 may or may not issue a bus transaction;
 * so we also try a 1-byte dummy read as fallback. */
static bool hs3003_probe(TwoWire* wire) {
    /* Try reading 1 byte — if sensor is present it will either ACK or
     * return a NACK only because it's mid-measurement (still present). */
    int got = wire->requestFrom((uint8_t)kSensorAddr, (uint8_t)1);
    return got > 0;
}

/* Trigger a measurement and read temperature.
 * Returns kBadTemp on bus error. */
static float hs3003_read_temp(TwoWire* wire) {
    /* Step 1: MR — trigger measurement (0-byte write). */
    wire->beginTransmission(kSensorAddr);
    /* endTransmission with 0 bytes sends START + addr + STOP on most
     * implementations; if it fails we still proceed to the read after delay. */
    wire->endTransmission();

    /* Step 2: wait for measurement to complete (≥ 33 ms). */
    delay(40);

    /* Step 3: DF — fetch 4 bytes. */
    int got = wire->requestFrom((uint8_t)kSensorAddr, (uint8_t)4);
    if (got != 4) return kBadTemp;

    uint8_t buf[4];
    for (int i = 0; i < 4; i++) buf[i] = wire->read();

    /* Check status bits (top 2 bits of byte 0): 0=valid, 1=stale, 2=busy. */
    uint8_t status = buf[0] >> 6;
    if (status == 2) return kBadTemp;  /* still measuring */

    /* Parse temperature: bits [23:10] of the 32-bit word. */
    uint16_t t_raw = ((uint16_t)buf[2] << 6) | (buf[3] >> 2);
    return (float)t_raw / 16383.0f * 165.0f - 40.0f;
}

/* =========================================================================
 * Bridge helper
 *
 * Calls a method on the Linux demo_app; responses are printed to Monitor
 * (visible in Arduino IDE Serial Monitor) and echoed to debug UART.
 * Requires arduino-router >= v0.7.0 on the board.
 * ====================================================================== */

static void bridge_call(const char* method, const char* arg) {
    String resp;
    Bridge.call(method, arg).result(resp);
    if (resp.length() == 0) return;
    Monitor.println(resp);
    Serial.println(resp);
}

/* =========================================================================
 * Arduino entry points
 * ====================================================================== */

void setup(void) {
    Serial.begin(115200);   /* debug UART */

    Bridge.begin();
    Monitor.begin();

    /* Modulino Thermo (HS3003) is on the Qwiic connector (Wire1 = i2c4). */
    Wire1.begin();
    if (hs3003_probe(&Wire1)) {
        g_wire      = &Wire1;
        g_sensor_ok = true;
    } else {
        Wire.begin();
        if (hs3003_probe(&Wire)) {
            g_wire      = &Wire;
            g_sensor_ok = true;
        }
    }

    /* Startup: send a status ping to Linux so demo_app logs the initial state. */
    bridge_call("demo/cmd", "s");
}

void loop(void) {
    static uint32_t last_tick = 0;
    uint32_t now = millis();

    /* Sensor tick — push temperature to Linux every kSensorIntervalMs. */
    if (now - last_tick >= kSensorIntervalMs) {
        last_tick = now;

        float t = kBadTemp;
        if (g_sensor_ok) {
            t = hs3003_read_temp(g_wire);
        }

        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%.4f", t);
        bridge_call("demo/tick", tbuf);
    }

    /* Forward Monitor keystrokes to Linux demo_app. */
    while (Monitor.available()) {
        int c = Monitor.read();
        if (c > 0 && c != '\n' && c != '\r') {
            char cmd[2] = { (char)c, '\0' };
            bridge_call("demo/cmd", cmd);
        }
    }
}
