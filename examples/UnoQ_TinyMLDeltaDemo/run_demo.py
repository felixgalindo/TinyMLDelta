#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_demo.py — Interactive end-to-end TinyMLDelta demo runner.

Walks through the full demo in order:
  1. Detect the Arduino UNO Q serial port.
  2. Training mode  — send 't', capture CSV, save training_data.csv.
  3. Model + patch  — run make_model.py to produce patch.tmd.
  4. Update mode    — send patch to device via Monitor serial link.
  5. Inference mode — stream live readings; watch for anomalies.

The Monitor channel on the UNO Q is routed through the board's Linux
co-processor (USB CDC gadget → socat → router RPC → STM32). The baud
rate you set doesn't affect throughput; pyserial opens the port at
whatever rate is specified and the USB framing handles the rest.

Usage:
  python3 run_demo.py [--port /dev/cu.usbmodem…] [--baud 115200]

Requirements:
  pip install tensorflow numpy pyserial

Author:  Felix Galindo
License: Apache-2.0
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import subprocess
import sys
import time
import textwrap

# ── Helpers ───────────────────────────────────────────────────────────────────

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
HR = "─" * 60


def hr(title: str = "") -> None:
    if title:
        pad = max(0, 58 - len(title))
        print(f"── {title} {'─' * pad}")
    else:
        print(HR)


def info(msg: str) -> None:  print(f"  {msg}")
def ok(msg: str)   -> None:  print(f"  ✓ {msg}")
def warn(msg: str) -> None:  print(f"  ⚠ {msg}")
def ask(prompt: str) -> str: return input(f"\n  {prompt} ").strip()


def require_serial():
    try:
        import serial
        return serial
    except ImportError:
        sys.exit("pyserial not found. Run:  pip install pyserial")


# ── Port detection ────────────────────────────────────────────────────────────

def detect_port() -> str | None:
    """Return the most likely UNO Q serial port, or None."""
    candidates: list[str] = []
    # macOS
    candidates += glob.glob("/dev/cu.usbmodem*")
    # Linux
    candidates += glob.glob("/dev/ttyACM*")
    # Windows
    candidates += glob.glob("COM[0-9]*")
    return candidates[0] if candidates else None


# ── Step 1: Training ──────────────────────────────────────────────────────────

CSV_BEGIN = "--- CSV BEGIN ---"
CSV_END   = "--- CSV END ---"
CSV_FILE  = os.path.join(SCRIPT_DIR, "training_data.csv")


def run_training(port: str, baud: int) -> str:
    """
    Send 't' to the device, capture the CSV block, return CSV text.
    Saves the CSV to training_data.csv automatically.
    """
    serial = require_serial()

    hr("STEP 1 — Training")
    info("Opening serial port ...")
    try:
        ser = serial.Serial(port, baud, timeout=2)
    except serial.SerialException as e:
        sys.exit(f"Cannot open port: {e}\n"
                 "  Hint: close the Arduino IDE Serial Monitor first.")

    time.sleep(1)
    ser.reset_input_buffer()

    info("Sending 't' to enter Training mode ...")
    ser.write(b"t\n")
    ser.flush()

    info("Collecting 200 samples (~100 s). Please wait ...\n")

    csv_lines: list[str] = []
    capturing = False
    last_progress = time.time()
    deadline = time.time() + 180  # 3-minute hard timeout

    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip()

        if CSV_BEGIN in line:
            capturing = True
            continue
        if CSV_END in line:
            break
        if capturing:
            csv_lines.append(line)
        else:
            # Print progress lines so the user sees something happening
            if "[TRAIN]" in line and time.time() - last_progress > 2:
                print(f"    {line}")
                last_progress = time.time()

    ser.close()

    if not csv_lines:
        sys.exit("No CSV data received. Is the sketch running and the sensor connected?")

    # First line should be 'temp_c' header; strip any extras
    csv_text = "\n".join(csv_lines) + "\n"

    with open(CSV_FILE, "w") as f:
        f.write(csv_text)

    sample_count = sum(1 for l in csv_lines if l and l[0].isdigit())
    ok(f"Captured {sample_count} samples → {CSV_FILE}")
    return CSV_FILE


# ── Step 2: Model + patch generation ─────────────────────────────────────────

PATCH_FILE  = os.path.join(SCRIPT_DIR, "patch.tmd")
MAKE_MODEL  = os.path.join(SCRIPT_DIR, "make_model.py")


def run_model_generation(csv_path: str) -> str:
    """Train autoencoder and generate patch.tmd. Returns patch path."""
    hr("STEP 2 — Model & patch generation")
    info(f"Running make_model.py with {csv_path} ...")
    print()

    result = subprocess.run(
        [sys.executable, MAKE_MODEL, "--csv", csv_path,
         "--out-dir", SCRIPT_DIR],
        cwd=SCRIPT_DIR,
    )
    if result.returncode != 0:
        sys.exit("make_model.py failed. See output above.")

    if not os.path.exists(PATCH_FILE):
        sys.exit(f"Expected patch file not found: {PATCH_FILE}")

    patch_size = os.path.getsize(PATCH_FILE)
    ok(f"patch.tmd generated  ({patch_size} bytes)")
    return PATCH_FILE


# ── Step 3: Update mode ───────────────────────────────────────────────────────

def run_update(port: str, baud: int, patch_path: str) -> None:
    """Send the patch to the device in Update mode."""
    serial = require_serial()

    hr("STEP 3 — OTA update")

    with open(patch_path, "rb") as f:
        patch_data = f.read()

    info(f"Opening serial port {port} ...")
    try:
        ser = serial.Serial(port, baud, timeout=2)
    except serial.SerialException as e:
        sys.exit(f"Cannot open port: {e}")

    time.sleep(1)
    ser.reset_input_buffer()

    # Enter Update mode
    info("Sending 'u' to enter Update mode ...")
    ser.write(b"u\n")
    ser.flush()
    time.sleep(0.5)

    # 4-byte LE length prefix
    ser.write(struct.pack("<I", len(patch_data)))
    ser.flush()
    time.sleep(0.1)

    # Patch bytes
    info(f"Streaming {len(patch_data)} patch bytes ...")
    ser.write(patch_data)
    ser.flush()

    # Wait for completion
    print()
    deadline = time.time() + 30
    success  = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip()
        print(f"    {line}")
        if "Done" in line or "active" in line or "inference" in line.lower():
            success = True
            break
        if "FAILED" in line or "Aborted" in line:
            break
        deadline = time.time() + 30  # reset on any activity

    ser.close()

    if success:
        ok("Patch applied successfully.")
    else:
        warn("Update did not confirm success — check output above.")


# ── Step 4: Inference ─────────────────────────────────────────────────────────

def run_inference(port: str, baud: int) -> None:
    """Send 'i', stream inference output until Ctrl-C."""
    serial = require_serial()

    hr("STEP 4 — Inference")
    info("Opening serial port ...")
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"Cannot open port: {e}")

    time.sleep(1)
    ser.reset_input_buffer()

    info("Sending 'i' to start inference ...")
    ser.write(b"i\n")
    ser.flush()

    print()
    info("Live readings (Ctrl-C to stop):")
    print()
    try:
        while True:
            raw = ser.readline()
            if raw:
                line = raw.decode(errors="replace").rstrip()
                marker = "  *** ANOMALY ***" if "ANOMALY" in line else ""
                print(f"    {line}{marker}")
    except KeyboardInterrupt:
        print()
        info("Inference stopped.")
    finally:
        ser.close()


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Interactive TinyMLDelta UNO Q demo runner.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Steps:
              1. Training  — collect sensor CSV from device
              2. Model     — train autoencoder, generate patch.tmd
              3. Update    — push patch to device over Monitor serial
              4. Inference — stream live anomaly-detection readings

            Prerequisites:
              • Sketch already uploaded (run setup.sh --upload first)
              • Arduino IDE Serial Monitor CLOSED (shares the serial port)
              • MCP9600 sensor connected via Qwiic cable
        """),
    )
    ap.add_argument("--port",  default=None, help="Serial port (auto-detected if omitted)")
    ap.add_argument("--baud",  type=int, default=115200, help="Baud rate (default: 115200)")
    ap.add_argument(
        "--skip-training", action="store_true",
        help=f"Skip training; use existing {CSV_FILE}",
    )
    ap.add_argument(
        "--skip-model", action="store_true",
        help=f"Skip model generation; use existing {PATCH_FILE}",
    )
    args = ap.parse_args()

    # ── Banner ────────────────────────────────────────────────────────────────
    print()
    hr()
    print("  TinyMLDelta — Arduino UNO Q Thermocouple Anomaly Demo")
    hr()
    print()
    print(textwrap.dedent("""\
        This script runs the full demo automatically:
          Train on-device → generate model+patch on PC → push update → infer

        Make sure:
          ✓ Sketch is uploaded  (run ./setup.sh --upload if needed)
          ✓ MCP9600 sensor is connected via Qwiic
          ✓ Arduino IDE Serial Monitor is CLOSED
    """))
    input("  Press Enter to continue...")

    # ── Port ─────────────────────────────────────────────────────────────────
    port = args.port or detect_port()
    if not port:
        sys.exit(
            "No serial port found.\n"
            "  • Is the Arduino UNO Q connected via USB?\n"
            "  • Specify manually:  python3 run_demo.py --port /dev/cu.usbmodem…"
        )
    ok(f"Using port: {port}")

    # ── Training ─────────────────────────────────────────────────────────────
    if args.skip_training:
        if not os.path.exists(CSV_FILE):
            sys.exit(f"--skip-training requested but {CSV_FILE} not found.")
        ok(f"Skipping training; using {CSV_FILE}")
        csv_path = CSV_FILE
    else:
        csv_path = run_training(port, args.baud)

    # ── Model generation ──────────────────────────────────────────────────────
    if args.skip_model:
        if not os.path.exists(PATCH_FILE):
            sys.exit(f"--skip-model requested but {PATCH_FILE} not found.")
        ok(f"Skipping model generation; using {PATCH_FILE}")
        patch_path = PATCH_FILE
    else:
        patch_path = run_model_generation(csv_path)

    # ── Update ────────────────────────────────────────────────────────────────
    run_update(port, args.baud, patch_path)

    # ── Inference ─────────────────────────────────────────────────────────────
    print()
    info("Tip: touch or breathe on the thermocouple probe to trigger an anomaly.")
    run_inference(port, args.baud)

    # ── Done ──────────────────────────────────────────────────────────────────
    print()
    hr()
    ok("Demo complete.")
    hr()
    print()


if __name__ == "__main__":
    main()
