#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
resnet8.py — MLPerf Tiny reference ResNet-8 (ResNetv1) for CIFAR-10.

This is the EEMBC / MLPerf Tiny image-classification reference architecture
(Banbury et al., 2021). ~78K parameters, ~96 KB int8 — a recognized, citable
TinyML model rather than a bespoke one. We use it so the benchmark stresses a
*real* deployed model with *real* field updates.

Author:  Felix Galindo
License: Apache-2.0
"""

from tensorflow import keras
from tensorflow.keras import layers


def build_resnet8(num_classes: int = 10, input_shape=(32, 32, 3)) -> keras.Model:
    """MLPerf Tiny ResNet-8 (ResNetv1): stem conv + 3 residual stacks + dense."""
    inp = keras.Input(shape=input_shape)

    # Stem
    x = layers.Conv2D(16, 3, padding="same", use_bias=False)(inp)
    x = layers.BatchNormalization()(x)
    x = layers.ReLU()(x)

    # Stack 1 — 16 filters, identity shortcut
    y = layers.Conv2D(16, 3, padding="same", use_bias=False)(x)
    y = layers.BatchNormalization()(y)
    y = layers.ReLU()(y)
    y = layers.Conv2D(16, 3, padding="same", use_bias=False)(y)
    y = layers.BatchNormalization()(y)
    x = layers.ReLU()(layers.Add()([x, y]))

    # Stack 2 — 32 filters, projection shortcut, stride-2 downsample
    y = layers.Conv2D(32, 3, strides=2, padding="same", use_bias=False)(x)
    y = layers.BatchNormalization()(y)
    y = layers.ReLU()(y)
    y = layers.Conv2D(32, 3, padding="same", use_bias=False)(y)
    y = layers.BatchNormalization()(y)
    sc = layers.Conv2D(32, 1, strides=2, padding="same", use_bias=False)(x)
    sc = layers.BatchNormalization()(sc)
    x = layers.ReLU()(layers.Add()([sc, y]))

    # Stack 3 — 64 filters, projection shortcut, stride-2 downsample
    y = layers.Conv2D(64, 3, strides=2, padding="same", use_bias=False)(x)
    y = layers.BatchNormalization()(y)
    y = layers.ReLU()(y)
    y = layers.Conv2D(64, 3, padding="same", use_bias=False)(y)
    y = layers.BatchNormalization()(y)
    sc = layers.Conv2D(64, 1, strides=2, padding="same", use_bias=False)(x)
    sc = layers.BatchNormalization()(sc)
    x = layers.ReLU()(layers.Add()([sc, y]))

    # Head
    x = layers.GlobalAveragePooling2D()(x)
    out = layers.Dense(num_classes, name="classifier_head")(x)

    model = keras.Model(inp, out, name="resnet8")
    return model


if __name__ == "__main__":
    m = build_resnet8()
    m.summary()
    print(f"\nTotal params: {m.count_params():,}")
