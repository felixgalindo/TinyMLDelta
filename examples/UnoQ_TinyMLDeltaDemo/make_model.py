#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_model.py — Train TFLite anomaly models and generate a TinyMLDelta patch.

Overview
--------
This script takes temperature training data collected from the Arduino sketch's
Training mode (CSV format) and produces:

  base.tflite        — Autoencoder trained on the provided normal-temperature data.
  target.tflite      — Fine-tuned version (slightly regularized) that will be the
                       update target shipped via TinyMLDelta.
  model_config.json  — Z-score normalization parameters (mean, std) for inference.
  patch.tmd          — TinyMLDelta patch (base → target) ready to send via ADB.

Model architecture
------------------
  Input:  4 normalized floats (sliding window of temperature readings)
  Hidden: Dense(8, relu)
  Bottleneck: Dense(4, relu)
  Output: Dense(4, linear)   ← reconstruction of input

  Anomaly score = mean squared error (MSE) between input and reconstruction.
  A reading is anomalous when score > kAnomalyThresholdMSE (0.04 by default).

  Ops used: FULLY_CONNECTED, RELU  — available in TFLite / libtensorflowlite_c.

Usage
-----
  python3 make_model.py training_data.csv

  Tune anomaly threshold:
      python3 make_model.py training_data.csv --threshold 0.03

  Custom output directory:
      python3 make_model.py training_data.csv --out-dir ./artifacts

Requirements:
    pip install tensorflow numpy

Author:  Felix Galindo
License: Apache-2.0
"""

import argparse
import json
import os
import sys
import textwrap

import numpy as np

# ---------------------------------------------------------------------------
# Constants — must match the sketch
# ---------------------------------------------------------------------------

WINDOW_SIZE  = 4          # kWindowSize in the sketch
HIDDEN_UNITS = 8          # hidden layer width
BOTTLENECK   = 4          # bottleneck / output width
EPOCHS_BASE  = 50
EPOCHS_FINE  = 20
BATCH_SIZE   = 32

# ---------------------------------------------------------------------------
# Data helpers
# ---------------------------------------------------------------------------

def zscore_normalize(temps: np.ndarray, mean: float, std: float) -> np.ndarray:
    """Z-score normalize temperatures using training baseline stats.

    Values near the training mean → ~0, out-of-distribution → large magnitude.
    The autoencoder learns to reconstruct near-zero inputs; anomalous temps
    produce values it has never seen, causing high reconstruction error.
    """
    if std < 1e-6:
        std = 1.0  # avoid division by zero for constant data
    return (temps - mean) / std


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
              2. Pull the CSV: adb pull /home/arduino/tinymldelta/training_data.csv .
              3. Run: python3 make_model.py training_data.csv
              4. Push files to board and apply:
                   adb push base.tflite /home/arduino/tinymldelta/model.tflite
                   adb push model_config.json /home/arduino/tinymldelta/model_config.json
                   adb push patch.tmd /home/arduino/tinymldelta/pending_patch.tmd
                   adb shell 'echo u > /home/arduino/tinymldelta/cmd.fifo'
        """),
    )
    ap.add_argument(
        "csv",
        metavar="CSV_FILE",
        help="CSV file with 'temp_c' column from Training mode.",
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

    # ---- Load training data ------------------------------------------------ #
    print(f"Loading training data from {args.csv}...")
    raw_temps = load_csv(args.csv)
    print(f"  {len(raw_temps)} samples loaded.")

    # Compute training baseline stats for z-score normalization.
    # Floor std at 3.0 C so the model tolerates natural room temp drift
    # (±3 C is normal) while flagging large deviations (touch, freezer).
    train_mean = float(np.mean(raw_temps))
    raw_std    = float(np.std(raw_temps))
    train_std  = max(raw_std, 3.0)
    print(f"  Baseline: mean={train_mean:.3f} C  std={train_std:.3f} C  (raw std={raw_std:.4f})")

    # Augment with smooth synthetic sequences at different offsets so the
    # autoencoder learns to reconstruct any stable temperature within ±2 std.
    # Each sequence is a smooth run at a shifted mean with small jitter.
    rng = np.random.default_rng(42)
    sequences = [raw_temps]
    for offset in np.linspace(-2.0, 2.0, 9):
        shifted = train_mean + offset * train_std + rng.normal(0, raw_std, len(raw_temps))
        sequences.append(shifted.astype(np.float32))
    augmented = np.concatenate(sequences)

    norm_temps = zscore_normalize(augmented, train_mean, train_std)
    windows    = make_windows(norm_temps, WINDOW_SIZE)
    print(f"  {len(windows)} training windows of size {WINDOW_SIZE} (augmented).")

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
    config_path = os.path.join(args.out_dir, "model_config.json")

    print("\nWriting output files:")

    with open(base_path, "wb") as f:
        f.write(base_tflite)
    print(f"  Wrote {base_path}  ({len(base_tflite)} bytes)")

    with open(target_path, "wb") as f:
        f.write(target_tflite)
    print(f"  Wrote {target_path}  ({len(target_tflite)} bytes)")

    # Save normalization config so demo_app uses the same z-score params
    config = {"mean": train_mean, "std": train_std, "window_size": WINDOW_SIZE}
    with open(config_path, "w") as f:
        json.dump(config, f, indent=2)
    print(f"  Wrote {config_path}  (mean={train_mean:.3f}, std={train_std:.3f})")

    print("\nGenerating TinyMLDelta patch (base → target)...")
    generate_patch(base_path, target_path, patch_path)

    # Check patch fits in kPatchBufSize on device
    if os.path.exists(patch_path):
        patch_size = os.path.getsize(patch_path)
        device_buf = 4096
        status = "OK" if patch_size <= device_buf else "WARNING: exceeds kPatchBufSize!"
        print(f"  Patch size: {patch_size} B / {device_buf} B buffer — {status}")

    print("\nDone. Next steps:")
    print("  1. Push the base model and config to the board:")
    print("       adb push base.tflite /home/arduino/tinymldelta/model.tflite")
    print("       adb push model_config.json /home/arduino/tinymldelta/model_config.json")
    print("  2. Push the patch and apply:")
    print("       adb push patch.tmd /home/arduino/tinymldelta/pending_patch.tmd")
    print("       adb shell 'echo u > /home/arduino/tinymldelta/cmd.fifo'")


if __name__ == "__main__":
    main()
