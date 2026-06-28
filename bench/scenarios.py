#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scenarios.py — Real-world model update scenarios for the TinyMLDelta benchmark.

Each scenario takes a trained *base* model and produces a *target* model the way
a real deployment would, so the resulting byte-level delta is representative of
an actual field update — not an artificial perturbation. These are the workflows
TinyMLDelta is designed for.

  S1  incremental_finetune  — continue training on new field data for N epochs
                              (the common "periodic retrain" case; drives the
                              patch-size-vs-divergence sweep).
  S2  domain_shift          — recover accuracy after real distribution drift
                              (dim/foggy/noisy camera). The headline field update.
  S3  head_update           — freeze the backbone, retrain only the classifier
                              head (on-device personalization / transfer learning).
                              Best case for delta patching: tiny update.
  S4  quant_recalibration   — identical weights, re-run int8 PTQ on drifted
                              calibration data. Isolates how much delta comes
                              purely from quantization parameter shift.

Author:  Felix Galindo
License: Apache-2.0
"""

import numpy as np
from tensorflow import keras

from resnet8 import build_resnet8


def _clone_from(base: keras.Model) -> keras.Model:
    """Fresh model initialized with the base model's weights."""
    m = build_resnet8(num_classes=base.output_shape[-1])
    m.set_weights(base.get_weights())
    return m


def _compile(m: keras.Model, lr: float):
    m.compile(
        optimizer=keras.optimizers.Adam(lr),
        loss=keras.losses.SparseCategoricalCrossentropy(from_logits=True),
        metrics=["accuracy"],
    )


def incremental_finetune(base, x_new, y_new, epochs: int, lr: float = 1e-4,
                         batch: int = 128, verbose: int = 0) -> keras.Model:
    """S1: continue training the whole network on new data for `epochs`."""
    m = _clone_from(base)
    _compile(m, lr)
    m.fit(x_new, y_new, epochs=epochs, batch_size=batch, verbose=verbose)
    return m


def domain_shift(base, x_drift, y_drift, epochs: int = 10, lr: float = 1e-4,
                 batch: int = 128, verbose: int = 0) -> keras.Model:
    """S2: recover accuracy on drifted field data (full-network fine-tune)."""
    m = _clone_from(base)
    _compile(m, lr)
    m.fit(x_drift, y_drift, epochs=epochs, batch_size=batch, verbose=verbose)
    return m


def head_update(base, x_new, y_new, epochs: int = 10, lr: float = 1e-3,
                batch: int = 128, verbose: int = 0) -> keras.Model:
    """S3: freeze backbone, retrain only the classifier head."""
    m = _clone_from(base)
    for layer in m.layers:
        layer.trainable = (layer.name == "classifier_head")
    _compile(m, lr)
    m.fit(x_new, y_new, epochs=epochs, batch_size=batch, verbose=verbose)
    return m
