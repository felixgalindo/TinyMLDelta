#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
data.py — CIFAR-10 loading and *realistic field drift* corruptions.

The corruptions mirror the CIFAR-10-C robustness benchmark
(Hendrycks & Dietterich, ICLR 2019): the kinds of distribution shift a
deployed camera actually experiences — dimmer light, fog/haze, sensor noise,
lower contrast, motion blur. These are what create the need for a *field
update* in the first place, which is exactly what TinyMLDelta ships.

No extra dependencies (numpy only) so the harness runs out of the box.

Author:  Felix Galindo
License: Apache-2.0
"""

import numpy as np
from tensorflow import keras


def _ensure_ssl():
    """macOS Python.framework ships without CA certs; use certifi if present."""
    try:
        import ssl
        import certifi
        ssl._create_default_https_context = lambda *a, **k: ssl.create_default_context(
            cafile=certifi.where())
    except Exception:
        pass


def load_cifar10():
    """Return (x_train, y_train), (x_test, y_test) as float32 in [0,1]."""
    _ensure_ssl()
    (xtr, ytr), (xte, yte) = keras.datasets.cifar10.load_data()
    xtr = xtr.astype("float32") / 255.0
    xte = xte.astype("float32") / 255.0
    return (xtr, ytr.flatten()), (xte, yte.flatten())


# --------------------------------------------------------------------------- #
#  Realistic corruptions (severity in 1..5, like CIFAR-10-C)                   #
# --------------------------------------------------------------------------- #

def _gaussian_kernel(sigma: float, radius: int = 2) -> np.ndarray:
    ax = np.arange(-radius, radius + 1)
    k = np.exp(-(ax ** 2) / (2 * sigma ** 2))
    k = k / k.sum()
    return k


def _blur(x: np.ndarray, sigma: float) -> np.ndarray:
    """Separable gaussian blur over H and W, per channel (numpy only)."""
    k = _gaussian_kernel(sigma).astype("float32")
    out = x.copy()
    # horizontal then vertical 1-D convolutions
    pad = len(k) // 2
    xp = np.pad(out, ((0, 0), (0, 0), (pad, pad), (0, 0)), mode="reflect")
    tmp = np.zeros_like(out)
    for i, w in enumerate(k):
        tmp += w * xp[:, :, i:i + out.shape[2], :]
    xp = np.pad(tmp, ((0, 0), (pad, pad), (0, 0), (0, 0)), mode="reflect")
    out = np.zeros_like(tmp)
    for i, w in enumerate(k):
        out += w * xp[:, i:i + tmp.shape[1], :, :]
    return out


def corrupt(x: np.ndarray, kind: str, severity: int = 3, seed: int = 0) -> np.ndarray:
    """Apply a named corruption at a given severity. Returns float32 in [0,1]."""
    rng = np.random.default_rng(seed)
    s = severity

    if kind == "brightness":          # deployment in dimmer/brighter light
        delta = [-0.4, -0.25, -0.1, 0.15, 0.3][s - 1]
        out = x + delta
    elif kind == "contrast":          # washed-out sensor
        factor = [0.4, 0.55, 0.7, 0.85, 1.3][s - 1]
        mean = x.mean(axis=(1, 2, 3), keepdims=True)
        out = (x - mean) * factor + mean
    elif kind == "gaussian_noise":    # cheap/aged image sensor
        std = [0.04, 0.08, 0.12, 0.18, 0.26][s - 1]
        out = x + rng.normal(0, std, x.shape).astype("float32")
    elif kind == "fog":               # haze / condensation on lens
        amt = [0.15, 0.3, 0.45, 0.6, 0.75][s - 1]
        out = x * (1 - amt) + amt  # blend toward white
    elif kind == "blur":              # defocus / motion
        sigma = [0.5, 0.8, 1.1, 1.5, 2.0][s - 1]
        out = _blur(x, sigma)
    else:
        raise ValueError(f"unknown corruption: {kind}")

    return np.clip(out, 0.0, 1.0).astype("float32")


# Default "field drift" preset: a realistic combination an outdoor camera sees.
DEFAULT_DRIFT = ["brightness", "fog", "gaussian_noise"]


def make_drift(x: np.ndarray, severity: int = 3, seed: int = 0) -> np.ndarray:
    """Compose the default real-world drift (dim + foggy + noisy)."""
    out = x
    for i, kind in enumerate(DEFAULT_DRIFT):
        out = corrupt(out, kind, severity=severity, seed=seed + i)
    return out
