#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_demo.py — End-to-end TinyMLDelta demo runner for the Arduino UNO Q.

Steps (fully automated):
  1. Training  — sends 't' via FIFO, waits for 200 samples, pulls CSV via ADB
  2. Model     — runs make_model.py on PC to train autoencoder + generate patch
  3. Update    — ADB-pushes base model + patch, sends 'u' via FIFO to apply
  4. Inference — sends 'i' via FIFO, streams live anomaly readings from log

All application logic (training, inference, patch apply) runs in demo_app
(C++ binary) on the UNO Q Linux co-processor.  The STM32 sketch is a thin
sensor proxy that reads the HS3003 temperature sensor.

Commands are sent via a named FIFO on the board:
  /home/arduino/tinymldelta/cmd.fifo

Training data is saved to:
  /home/arduino/tinymldelta/training_data.csv

Usage:
  python3 run_demo.py [--skip-training] [--skip-model]

Prerequisites (handled by setup.sh):
  - demo_app deployed to board  (./linux/deploy_service.sh)
  - Sketch uploaded              (./setup.sh)
  - HS3003 sensor on Qwiic cable
  - ADB accessible

Author:  Felix Galindo
License: Apache-2.0
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import textwrap
import time

# ── Helpers ───────────────────────────────────────────────────────────────────

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
REMOTE_DIR  = "/home/arduino/tinymldelta"
CMD_FIFO    = f"{REMOTE_DIR}/cmd.fifo"
REMOTE_CSV  = f"{REMOTE_DIR}/training_data.csv"
REMOTE_LOG  = f"{REMOTE_DIR}/demo_app.log"
HR          = "─" * 60

# ADB path — matches arduino-cli tools install location
ADB_DEFAULT = os.path.expanduser(
    "~/Library/Arduino15/packages/arduino/tools/adb/32.0.0/adb"
)


def find_adb() -> str:
    """Find ADB binary."""
    if os.path.isfile(ADB_DEFAULT) and os.access(ADB_DEFAULT, os.X_OK):
        return ADB_DEFAULT
    # Try PATH
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, "adb")
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    sys.exit("ADB not found. Install via arduino-cli or add to PATH.")


def adb_cmd(adb: str, shell_cmd: str, check: bool = True) -> str:
    """Run an ADB shell command and return stdout."""
    result = subprocess.run(
        [adb, "shell", shell_cmd],
        capture_output=True, text=True,
    )
    if check and result.returncode != 0:
        sys.exit(f"ADB command failed: {shell_cmd}\n{result.stderr}")
    return result.stdout.strip()


def hr(title: str = "") -> None:
    if title:
        pad = max(0, 58 - len(title))
        print(f"── {title} {'─' * pad}")
    else:
        print(HR)


def info(msg: str) -> None: print(f"  {msg}")
def ok(msg: str)   -> None: print(f"  [OK] {msg}")
def warn(msg: str) -> None: print(f"  [!!] {msg}")


# ── Step 1: Training ────────────────────────────────────────────────────────

CSV_FILE   = os.path.join(SCRIPT_DIR, "training_data.csv")
MAX_WAIT_S = 150  # max seconds to wait for training to complete


def run_training(adb: str) -> str:
    """Send 't' via FIFO, wait for 200 samples, pull CSV via ADB."""
    hr("STEP 1 — Training")

    # Check demo_app is running
    out = adb_cmd(adb, "pgrep -x demo_app", check=False)
    if not out:
        sys.exit("demo_app is not running. Deploy with: ./linux/deploy_service.sh")

    info("Sending 't' → collecting 200 samples (~100 s) ...")
    adb_cmd(adb, f"echo t > {CMD_FIFO}")
    print()

    start = time.time()
    last_status = start
    while time.time() - start < MAX_WAIT_S:
        time.sleep(5)

        # Check if CSV file exists and has 201 lines (header + 200 samples)
        line_count = adb_cmd(adb, f"wc -l < {REMOTE_CSV} 2>/dev/null || echo 0", check=False)
        try:
            n = int(line_count.strip())
        except ValueError:
            n = 0

        if n >= 201:
            break

        if time.time() - last_status > 15:
            info(f"  {max(0, n - 1)}/200 samples collected ...")
            last_status = time.time()
    else:
        warn("Training timed out. Check demo_app logs.")

    # Pull CSV
    info("Pulling training_data.csv from board ...")
    result = subprocess.run(
        [adb, "pull", REMOTE_CSV, CSV_FILE],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.exit(f"Failed to pull CSV: {result.stderr}")

    # Count samples
    with open(CSV_FILE) as f:
        lines = [l for l in f if l.strip() and l[0].isdigit()]
    ok(f"Captured {len(lines)} samples → training_data.csv")
    return CSV_FILE


# ── Step 2: Model + patch generation ─────────────────────────────────────────

PATCH_FILE  = os.path.join(SCRIPT_DIR, "patch.tmd")
BASE_FILE   = os.path.join(SCRIPT_DIR, "base.tflite")
CONFIG_FILE = os.path.join(SCRIPT_DIR, "model_config.json")
MAKE_MODEL  = os.path.join(SCRIPT_DIR, "make_model.py")


def run_model_generation(csv_path: str) -> str:
    """Train autoencoder on PC and generate patch.tmd."""
    hr("STEP 2 — Model & patch generation")
    info("Running make_model.py ...")
    print()

    result = subprocess.run(
        [sys.executable, MAKE_MODEL, csv_path, "--out-dir", SCRIPT_DIR],
        cwd=SCRIPT_DIR,
    )
    if result.returncode != 0:
        sys.exit("make_model.py failed. See output above.")

    if not os.path.exists(PATCH_FILE):
        sys.exit(f"Expected patch file not found: {PATCH_FILE}")

    ok(f"patch.tmd generated  ({os.path.getsize(PATCH_FILE)} bytes)")
    return PATCH_FILE


# ── Step 3: OTA update ──────────────────────────────────────────────────────

def run_update(adb: str, patch_path: str) -> None:
    """ADB-push base model + patch, then send 'u' via FIFO to apply."""
    hr("STEP 3 — OTA update")

    # Push base model
    if os.path.exists(BASE_FILE):
        info(f"Pushing base model ({os.path.getsize(BASE_FILE)} B) ...")
        subprocess.run([adb, "push", BASE_FILE, f"{REMOTE_DIR}/model.tflite"],
                       capture_output=True, check=True)

    # Push normalization config (z-score mean/std from training)
    if os.path.exists(CONFIG_FILE):
        info(f"Pushing model_config.json ...")
        subprocess.run([adb, "push", CONFIG_FILE, f"{REMOTE_DIR}/model_config.json"],
                       capture_output=True, check=True)

    # Push patch
    info(f"Pushing patch ({os.path.getsize(patch_path)} B) ...")
    subprocess.run([adb, "push", patch_path, f"{REMOTE_DIR}/pending_patch.tmd"],
                   capture_output=True, check=True)

    # apply_patch() reads the base model from disk, so no restart needed.
    info("Sending 'u' to apply patch ...")
    adb_cmd(adb, f"echo u > {CMD_FIFO}")
    time.sleep(3)

    # Check result
    log = adb_cmd(adb, f"grep -E 'Patch applied|FAILED|Updated model' {REMOTE_LOG} | tail -3",
                  check=False)
    print()
    for line in log.splitlines():
        info(f"  {line}")
    print()

    if "Patch applied" in log and "Updated model" in log:
        ok("Patch applied successfully.")
    else:
        warn("Patch apply may have failed — check logs.")


# ── Step 4: Inference ───────────────────────────────────────────────────────

def run_inference(adb: str) -> None:
    """Send 'i' via FIFO, stream live anomaly readings from log."""
    hr("STEP 4 — Inference")
    info("Sending 'i' — live readings (Ctrl-C to stop):")
    adb_cmd(adb, f"echo i > {CMD_FIFO}")
    time.sleep(2)
    print()

    info("Tip: warm the sensor to trigger an anomaly.")
    print()

    try:
        proc = subprocess.Popen(
            [adb, "shell", f"tail -f {REMOTE_LOG}"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True,
        )
        for line in proc.stdout:
            line = line.rstrip()
            if "TEMP" in line or "ANOMALY" in line:
                print(f"    {line}")
    except KeyboardInterrupt:
        print()
        info("Inference stopped.")
    finally:
        if proc.poll() is None:
            proc.terminate()


# ── Main ────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description="End-to-end TinyMLDelta UNO Q demo runner.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Prerequisites (all handled by setup.sh):
              - demo_app deployed  (./linux/deploy_service.sh)
              - Sketch uploaded    (./setup.sh)
              - HS3003 on Qwiic cable
        """),
    )
    ap.add_argument("--skip-training", action="store_true",
                    help="Skip training; use existing training_data.csv")
    ap.add_argument("--skip-model", action="store_true",
                    help="Skip model gen; use existing patch.tmd")
    args = ap.parse_args()

    # ── Banner ────────────────────────────────────────────────────────────
    print()
    hr()
    print("  TinyMLDelta — Arduino UNO Q Temperature Anomaly Demo")
    hr()
    print()

    # ── ADB ──────────────────────────────────────────────────────────────
    adb = find_adb()
    ok(f"ADB: {adb}")
    print()

    # ── Training ─────────────────────────────────────────────────────────
    if args.skip_training:
        if not os.path.exists(CSV_FILE):
            sys.exit(f"--skip-training: {CSV_FILE} not found.")
        ok(f"Skipping training; using {CSV_FILE}")
        csv_path = CSV_FILE
    else:
        csv_path = run_training(adb)

    print()

    # ── Model generation ─────────────────────────────────────────────────
    if args.skip_model:
        if not os.path.exists(PATCH_FILE):
            sys.exit(f"--skip-model: {PATCH_FILE} not found.")
        ok(f"Skipping model gen; using {PATCH_FILE}")
        patch_path = PATCH_FILE
    else:
        patch_path = run_model_generation(csv_path)

    print()

    # ── Update ───────────────────────────────────────────────────────────
    run_update(adb, patch_path)
    print()

    # ── Inference ────────────────────────────────────────────────────────
    run_inference(adb)

    # ── Done ─────────────────────────────────────────────────────────────
    print()
    hr()
    ok("Demo complete.")
    hr()
    print()


if __name__ == "__main__":
    main()
