#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file make_models.py
@brief TinyMLDelta Example — Generate realistic MCU-sized sensor models.
@author Felix Galindo
@license Apache-2.0

This script builds a small-but-realistic Conv1D-based sensor ML classifier,
representative of models deployed on microcontrollers for IMU motion detection,
keyword spotting, or anomaly classification.

It produces:
    base.tflite   – initial model (Conv1D + Dense)
    target.tflite – same architecture, slightly nudged weights

Users can test TinyMLDelta patch generation and embedded delta-apply flows
without training or collecting data.

Usage:
    python3 make_models.py

Outputs:
    base.tflite
    target.tflite
"""

import tensorflow as tf
import numpy as np


# -----------------------------------------------------------------------------
# Model builder
# -----------------------------------------------------------------------------
def build_sensor_model():
    """
    Build a realistic MCU-sized Conv1D + Dense model.

    Input shape: 64 timesteps x 3 channels (e.g., IMU window).
    Architecture:
        Conv1D(16) → Conv1D(32) → Flatten → Dense(32) → Dense(3)
    This keeps parameter count modest so the TFLite flatbuffer fits
    comfortably inside a 128 KiB firmware slot.
    """
    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(64, 3), name="input"),
        tf.keras.layers.Conv1D(
            16, kernel_size=5, strides=1,
            activation="relu", name="conv1"
        ),
        tf.keras.layers.Conv1D(
            32, kernel_size=3, strides=1,
            activation="relu", name="conv2"
        ),
        tf.keras.layers.Flatten(name="flatten"),
        tf.keras.layers.Dense(32, activation="relu", name="dense1"),
        tf.keras.layers.Dense(3, activation=None, name="dense2"),  # 3-class classifier
    ])
    return model


# -----------------------------------------------------------------------------
# TFLite export helper
# -----------------------------------------------------------------------------
def save_tflite(keras_model, path: str):
    """
    Convert a Keras model to TFLite and write it to disk.

    Args:
        keras_model: Keras model instance.
        path: Output .tflite filename.
    """
    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
    # Keep size small for MCUs; enables weight quantization and other tricks.
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    tflite_model = converter.convert()

    with open(path, "wb") as f:
        f.write(tflite_model)

    print(f"[ModelGen] Saved {path} ({len(tflite_model)} bytes)")


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main():
    print("[ModelGen] Building base sensor model...")

    # 1) Build the base model.
    base_model = build_sensor_model()

    # Export baseline version.
    save_tflite(base_model, "base.tflite")

    # 2) Create a “target” model by nudging the final Dense layer weights.
    print("[ModelGen] Nudging dense2 weights to create target version...")
    dense2 = base_model.get_layer("dense2")
    w, b = dense2.get_weights()

    # Minor random perturbation ensures small patch sizes while still
    # exercising the diff logic in TinyMLDelta.
    w2 = w + 0.005 * np.random.randn(*w.shape).astype(w.dtype)
    dense2.set_weights([w2, b])

    # Export updated version.
    save_tflite(base_model, "target.tflite")

    print("\n[ModelGen] Done.")
    print("Generated: base.tflite and target.tflite")


if __name__ == "__main__":
    main()
