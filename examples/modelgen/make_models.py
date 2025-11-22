#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TinyMLDelta Example — Generate Two Tiny TFLite Models
=====================================================

This script creates two minimal TFLite models:

    base.tflite   – initial model
    target.tflite – same architecture, slightly nudged weights

This lets users test TinyMLDelta patch generation and apply flows
without needing to build or train any ML model.

Works on macOS, Linux, and Windows (with Python + TensorFlow).

Usage:
    python3 make_models.py

Outputs:
    base.tflite
    target.tflite
"""

import tensorflow as tf
import numpy as np


def build_model():
    """Create a tiny 1-layer dense model."""
    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(4,), name="input"),
        tf.keras.layers.Dense(3, activation=None, name="dense")
    ])
    return model


def save_tflite(keras_model, path: str):
    """Convert the Keras model to TFLite and save to disk."""
    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    tflite_model = converter.convert()
    with open(path, "wb") as f:
        f.write(tflite_model)
    print(f"[ModelGen] Saved {path} ({len(tflite_model)} bytes)")


def main():
    # 1) Build the base model
    base_model = build_model()

    # Save baseline version
    save_tflite(base_model, "base.tflite")

    # 2) Create a “target” model by nudging weights slightly
    dense = base_model.get_layer("dense")
    w, b = dense.get_weights()
    w2 = w + 0.01 * np.random.randn(*w.shape).astype(w.dtype)
    dense.set_weights([w2, b])

    # Save updated version
    save_tflite(base_model, "target.tflite")

    print("\n[ModelGen] Done.")
    print("Generated: base.tflite  and  target.tflite")


if __name__ == "__main__":
    main()
