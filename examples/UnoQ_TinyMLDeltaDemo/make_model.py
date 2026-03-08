#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_model.py — Train TFLite anomaly models and generate a TinyMLDelta patch.

Overview
--------
This script takes temperature training data collected from the Arduino sketch's
Training mode (CSV format) and produces:

  base.tflite   — Autoencoder trained on the provided normal-temperature data.
  target.tflite — Fine-tuned version (slightly regularized) that will be the
                  update target shipped via TinyMLDelta.
  model.h       — C header embedding base.tflite as a byte array for the sketch.
  patch.tmd     — TinyMLDelta patch (base → target) ready to send via serial.

Model architecture
------------------
  Input:  4 normalized floats (sliding window of temperature readings)
  Hidden: Dense(8, relu)
  Bottleneck: Dense(4, relu)
  Output: Dense(4, linear)   ← reconstruction of input

  Anomaly score = mean squared error (MSE) between input and reconstruction.
  A reading is anomalous when score > kAnomalyThresholdMSE (0.04 by default).

  Ops used: FULLY_CONNECTED, RELU  — both available in Arduino_TensorFlowLite.

Usage
-----
  Basic (synthetic normal data):
      python3 make_model.py

  With real training data from the device:
      python3 make_model.py --csv training_data.csv

  Tune anomaly threshold:
      python3 make_model.py --csv training_data.csv --threshold 0.03

  Custom output directory:
      python3 make_model.py --csv training_data.csv --out-dir ./artifacts

Requirements:
    pip install tensorflow numpy

Author:  Felix Galindo
License: Apache-2.0
"""

import argparse
import os
import struct
import sys
import textwrap

import numpy as np

# ---------------------------------------------------------------------------
# Constants — must match the sketch
# ---------------------------------------------------------------------------

WINDOW_SIZE  = 4          # kWindowSize in the sketch
TEMP_MIN     = -10.0      # kTempMin
TEMP_MAX     =  85.0      # kTempMax
HIDDEN_UNITS = 8          # hidden layer width
BOTTLENECK   = 4          # bottleneck / output width
EPOCHS_BASE  = 50
EPOCHS_FINE  = 20
BATCH_SIZE   = 32

# ---------------------------------------------------------------------------
# Data helpers
# ---------------------------------------------------------------------------

def normalize(temps: np.ndarray) -> np.ndarray:
    """Normalize temperatures to [0, 1] using the sketch's fixed range."""
    v = (temps - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)
    return np.clip(v, 0.0, 1.0)


def make_windows(temps: np.ndarray, window: int) -> np.ndarray:
    """Convert a 1-D temperature array into overlapping windows."""
    if len(temps) < window:
        raise ValueError(
            f"Need at least {window} samples; got {len(temps)}."
        )
    return np.array(
        [temps[i:i + window] for i in range(len(temps) - window + 1)],
        dtype=np.float32,
    )


def load_csv(path: str) -> np.ndarray:
    """Load a CSV file with a 'temp_c' column (as produced by Training mode)."""
    import csv
    temps = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            temps.append(float(row["temp_c"]))
    return np.array(temps, dtype=np.float32)


def make_synthetic_data(n: int = 500, mean: float = 25.0, std: float = 2.0) -> np.ndarray:
    """Generate synthetic normal-temperature data (Gaussian)."""
    rng = np.random.default_rng(42)
    return rng.normal(mean, std, n).astype(np.float32)

# ---------------------------------------------------------------------------
# Model definition
# ---------------------------------------------------------------------------

def build_autoencoder():
    """Return a compiled Keras autoencoder for temperature window reconstruction."""
    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow not found. Run:  pip install tensorflow")

    inp = tf.keras.Input(shape=(WINDOW_SIZE,))
    x   = tf.keras.layers.Dense(HIDDEN_UNITS, activation="relu")(inp)
    x   = tf.keras.layers.Dense(BOTTLENECK,   activation="relu")(x)
    out = tf.keras.layers.Dense(WINDOW_SIZE)(x)           # linear reconstruction

    model = tf.keras.Model(inp, out)
    model.compile(optimizer="adam", loss="mse")
    return model

# ---------------------------------------------------------------------------
# TFLite conversion
# ---------------------------------------------------------------------------

def to_tflite(keras_model) -> bytes:
    """Convert a Keras model to a TFLite flatbuffer (float32, no quantization)."""
    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow not found.")

    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
    return converter.convert()


def tflite_to_c_array(data: bytes, array_name: str = "kBaseModelData") -> str:
    """Format TFLite flatbuffer bytes as a C byte-array literal."""
    hex_vals = ", ".join(f"0x{b:02x}" for b in data)
    # Wrap at 80 chars
    wrapped = textwrap.fill(hex_vals, width=76, subsequent_indent="    ")
    return (
        f"static const uint8_t {array_name}[] = {{\n"
        f"    {wrapped}\n"
        f"}};\n"
        f"static const size_t  k{''.join(array_name[1:].split('Data'))}Size"
        f" = sizeof({array_name});\n"
    )


def write_model_header(tflite_bytes: bytes, out_path: str) -> None:
    """Write model.h with the embedded TFLite model and generation instructions."""
    c_array = tflite_to_c_array(tflite_bytes, "kBaseModelData")
    with open(out_path, "w") as f:
        f.write(
            "/*\n"
            " * model.h — Base TFLite model (generated by make_model.py)\n"
            " *\n"
            " * Autoencoder architecture:\n"
            f" *   Input:  {WINDOW_SIZE} normalized floats\n"
            f" *   Hidden: Dense({HIDDEN_UNITS}, relu)\n"
            f" *   Bottleneck: Dense({BOTTLENECK}, relu)\n"
            f" *   Output: Dense({WINDOW_SIZE}, linear)\n"
            " *\n"
            " * Anomaly score = MSE(input, reconstruction).\n"
            " * Regenerate with:  python3 make_model.py --csv training_data.csv\n"
            " */\n"
            "\n"
            "#pragma once\n"
            "#include <stdint.h>\n"
            "\n"
            + c_array
        )
    print(f"  Wrote {out_path}  ({len(tflite_bytes)} bytes)")

# ---------------------------------------------------------------------------
# Patch generation (calls TinyMLDelta PatchGen)
# ---------------------------------------------------------------------------

def generate_patch(base_path: str, target_path: str, patch_path: str) -> None:
    """Call tinymldelta_patchgen.py to create patch.tmd."""
    # Locate patchgen relative to this script
    script_dir  = os.path.dirname(os.path.abspath(__file__))
    cli_dir     = os.path.join(script_dir, "..", "..", "cli")
    patchgen    = os.path.join(cli_dir, "tinymldelta_patchgen.py")

    if not os.path.exists(patchgen):
        print(f"  [WARN] tinymldelta_patchgen.py not found at {patchgen}")
        print("  Patch not generated. Run patchgen manually.")
        return

    import subprocess
    cmd = [
        sys.executable, patchgen,
        base_path, target_path, patch_path,
        "--algo", "crc32",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        print(f"  [WARN] patchgen exited with code {result.returncode}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Train TFLite anomaly autoencoder and generate TinyMLDelta patch.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Workflow:
              1. In Training mode on the device, collect CSV output.
              2. Save the CSV block (between markers) to training_data.csv.
              3. Run: python3 make_model.py --csv training_data.csv
              4. Re-flash the sketch (model.h updated).
              5. To push an update over serial: python3 send_patch.py <port> patch.tmd
        """),
    )
    ap.add_argument(
        "--csv",
        metavar="FILE",
        default=None,
        help="CSV file with 'temp_c' column from Training mode. "
             "If omitted, synthetic Gaussian data is used.",
    )
    ap.add_argument(
        "--mean",
        type=float,
        default=25.0,
        help="Mean for synthetic data (°C, default: 25.0)",
    )
    ap.add_argument(
        "--std",
        type=float,
        default=2.0,
        help="Std-dev for synthetic data (°C, default: 2.0)",
    )
    ap.add_argument(
        "--threshold",
        type=float,
        default=0.04,
        help="Anomaly MSE threshold to print (informational, default: 0.04)",
    )
    ap.add_argument(
        "--out-dir",
        default=".",
        metavar="DIR",
        help="Output directory (default: current directory)",
    )
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    # ---- Load or generate training data ---------------------------------- #
    if args.csv:
        print(f"Loading training data from {args.csv}...")
        raw_temps = load_csv(args.csv)
        print(f"  {len(raw_temps)} samples loaded.")
    else:
        print(f"No CSV provided. Generating synthetic data (mean={args.mean} C, std={args.std} C)...")
        raw_temps = make_synthetic_data(500, mean=args.mean, std=args.std)

    norm_temps = normalize(raw_temps)
    windows    = make_windows(norm_temps, WINDOW_SIZE)
    print(f"  {len(windows)} training windows of size {WINDOW_SIZE}.")

    # ---- Build and train base model -------------------------------------- #
    print("\nTraining base autoencoder...")
    base_model = build_autoencoder()
    base_model.fit(
        windows, windows,
        epochs=EPOCHS_BASE,
        batch_size=BATCH_SIZE,
        validation_split=0.1,
        verbose=0,
    )

    # Evaluate reconstruction error on training set
    reconstructed = base_model.predict(windows, verbose=0)
    mse_per_sample = np.mean((windows - reconstructed) ** 2, axis=1)
    print(f"  Base model train MSE: mean={mse_per_sample.mean():.5f}  "
          f"max={mse_per_sample.max():.5f}  (threshold={args.threshold})")

    # ---- Build target model (fine-tuned with L2 regularization) ---------- #
    # The target model is a slightly different version that would have lower
    # reconstruction error on an updated temperature distribution. In a real
    # deployment you would retrain on new data from the field.
    print("\nFine-tuning target model...")

    import tensorflow as tf

    inp = tf.keras.Input(shape=(WINDOW_SIZE,))
    x   = tf.keras.layers.Dense(
        HIDDEN_UNITS, activation="relu",
        kernel_regularizer=tf.keras.regularizers.l2(1e-4),
    )(inp)
    x   = tf.keras.layers.Dense(
        BOTTLENECK, activation="relu",
        kernel_regularizer=tf.keras.regularizers.l2(1e-4),
    )(x)
    out = tf.keras.layers.Dense(WINDOW_SIZE)(x)
    target_model = tf.keras.Model(inp, out)
    target_model.compile(optimizer=tf.keras.optimizers.Adam(1e-4), loss="mse")

    # Initialize from base weights then fine-tune
    target_model.set_weights(base_model.get_weights())
    target_model.fit(
        windows, windows,
        epochs=EPOCHS_FINE,
        batch_size=BATCH_SIZE,
        validation_split=0.1,
        verbose=0,
    )
    reconstructed2 = target_model.predict(windows, verbose=0)
    mse2 = np.mean((windows - reconstructed2) ** 2, axis=1)
    print(f"  Target model train MSE: mean={mse2.mean():.5f}  max={mse2.max():.5f}")

    # ---- Convert to TFLite ----------------------------------------------- #
    print("\nConverting to TFLite...")
    base_tflite   = to_tflite(base_model)
    target_tflite = to_tflite(target_model)

    # ---- Write files ------------------------------------------------------- #
    base_path   = os.path.join(args.out_dir, "base.tflite")
    target_path = os.path.join(args.out_dir, "target.tflite")
    patch_path  = os.path.join(args.out_dir, "patch.tmd")
    header_path = os.path.join(args.out_dir, "model.h")

    print("\nWriting output files:")

    with open(base_path, "wb") as f:
        f.write(base_tflite)
    print(f"  Wrote {base_path}  ({len(base_tflite)} bytes)")

    with open(target_path, "wb") as f:
        f.write(target_tflite)
    print(f"  Wrote {target_path}  ({len(target_tflite)} bytes)")

    write_model_header(base_tflite, header_path)

    print("\nGenerating TinyMLDelta patch (base → target)...")
    generate_patch(base_path, target_path, patch_path)

    # Check patch fits in kPatchBufSize on device
    if os.path.exists(patch_path):
        patch_size = os.path.getsize(patch_path)
        device_buf = 4096
        status = "OK" if patch_size <= device_buf else "WARNING: exceeds kPatchBufSize!"
        print(f"  Patch size: {patch_size} B / {device_buf} B buffer — {status}")

    print("\nDone. Next steps:")
    print("  1. Copy model.h into your Arduino sketch folder (if not already there).")
    print("  2. Re-upload the sketch to load the new base model.")
    print("  3. Send the update live with:")
    print("       python3 send_patch.py <serial-port> patch.tmd")


if __name__ == "__main__":
    main()
