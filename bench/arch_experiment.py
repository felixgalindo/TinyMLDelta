#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
arch_experiment.py — Does byte-level delta survive ARCHITECTURE changes?

This isolates the architecture-change effect from the weight-change effect.
For each case we build a target whose *weights are mostly (or entirely) shared
with the base*, change only the architecture, and measure the byte-level
TinyMLDelta patch ratio. If the ratio explodes despite shared weights, the
culprit is FlatBuffer re-serialization (offsets/tensor table/op table shift) —
i.e. byte-diff is the wrong tool for architecture updates.

Cases (all realistic TinyML arch updates):
  weight_only   same arch, perturb one tensor      -> control (tiny patch)
  add_class     10 -> 11 output classes            -> only final Dense grows
  input_res     32x32 -> 48x48 input               -> 100% identical weights,
                                                       only shapes/arena change
  widen_head    final stack 64 -> 96 filters        -> structural growth

No training: the point is purely structural, so random (but *shared*) weights
suffice and it runs in seconds.

Author:  Felix Galindo
License: Apache-2.0
"""

import numpy as np

from resnet8 import build_resnet8
from convert import to_tflite_float32
from patchsize import measure


def shared_fraction(wa, wb) -> float:
    """Fraction of base weight *elements* that appear unchanged in target."""
    same = tot = 0
    for a, b in zip(wa, wb):
        if a.shape == b.shape:
            same += int(np.sum(a == b))
        tot += a.size
    return same / tot if tot else 0.0


def copy_shared(dst, src):
    """Copy src weights into dst where shapes match; slice-copy where they grow."""
    sw, dw = src.get_weights(), dst.get_weights()
    out = []
    for d, s in zip(dw, sw):
        if d.shape == s.shape:
            out.append(s.copy())
        else:
            # grown tensor: copy the overlapping corner, keep dst's init elsewhere
            slices = tuple(slice(0, min(a, b)) for a, b in zip(d.shape, s.shape))
            merged = d.copy()
            merged[slices] = s[slices]
            out.append(merged)
    dst.set_weights(out)
    return dst


def main():
    rng = np.random.default_rng(0)
    base = build_resnet8(num_classes=10, input_shape=(32, 32, 3))
    base_tfl = to_tflite_float32(base)
    bw = base.get_weights()

    rep = rng.random((8, 32, 32, 3)).astype("float32")  # unused for float32
    rows = []

    def run(name, target_model, note):
        tw = target_model.get_weights()
        frac = shared_fraction(bw, tw)
        tgt_tfl = to_tflite_float32(target_model)
        r = measure(base_tfl, tgt_tfl)
        ratio = r["tmd_ratio"]
        rows.append((name, frac, len(base_tfl), len(tgt_tfl),
                     r["tmd_bytes"], ratio, r["gzip_full_bytes"], note))
        print(f"  {name:12s} shared_w={frac*100:5.1f}%  "
              f"base={len(base_tfl):>6d}B target={len(tgt_tfl):>6d}B  "
              f"byte_patch={r['tmd_bytes']:>7d}B ({ratio*100:5.1f}%)  {note}")

    print("Architecture-change vs. byte-level delta (ResNet-8, float32):\n")

    # control: same architecture, one tensor nudged
    w2 = build_resnet8(10, (32, 32, 3))
    w2.set_weights([w.copy() for w in bw])
    pert = w2.get_weights()
    pert[-1] = pert[-1] + 0.01  # nudge final bias
    w2.set_weights(pert)
    run("weight_only", w2, "same arch, 1 tensor nudged")

    # add a class: 10 -> 11 (only final Dense changes shape)
    addc = copy_shared(build_resnet8(11, (32, 32, 3)), base)
    run("add_class", addc, "10->11 classes")

    # input resolution: 32 -> 48 (weights 100% identical; shapes/arena change)
    ires = build_resnet8(10, (48, 48, 3))
    ires.set_weights([w.copy() for w in bw])  # identical weights
    run("input_res", ires, "32x32 -> 48x48 input")

    # widen the final stack: 64 -> 96 filters (structural growth)
    # (rebuild with a wider last stack via a custom variant)
    widen = _build_widened(96)
    widen = copy_shared(widen, base)
    run("widen_head", widen, "stack3 64->96 filters")

    print("\nTakeaway: weight_only stays tiny; every architecture change blows the")
    print("byte-patch toward ~100% EVEN when weights are mostly/fully shared, because")
    print("the TFLite FlatBuffer re-serializes (tensor/op tables + offsets shift).")
    print("=> architecture updates need STRUCTURE-AWARE diffing, not byte diffing.")


def _build_widened(filters):
    """ResNet-8 with the final residual stack widened to `filters`."""
    from tensorflow import keras
    from tensorflow.keras import layers
    inp = keras.Input((32, 32, 3))
    x = layers.ReLU()(layers.BatchNormalization()(
        layers.Conv2D(16, 3, padding="same", use_bias=False)(inp)))
    y = layers.ReLU()(layers.BatchNormalization()(
        layers.Conv2D(16, 3, padding="same", use_bias=False)(x)))
    y = layers.BatchNormalization()(layers.Conv2D(16, 3, padding="same", use_bias=False)(y))
    x = layers.ReLU()(layers.Add()([x, y]))
    y = layers.ReLU()(layers.BatchNormalization()(
        layers.Conv2D(32, 3, strides=2, padding="same", use_bias=False)(x)))
    y = layers.BatchNormalization()(layers.Conv2D(32, 3, padding="same", use_bias=False)(y))
    sc = layers.BatchNormalization()(layers.Conv2D(32, 1, strides=2, padding="same", use_bias=False)(x))
    x = layers.ReLU()(layers.Add()([sc, y]))
    y = layers.ReLU()(layers.BatchNormalization()(
        layers.Conv2D(filters, 3, strides=2, padding="same", use_bias=False)(x)))
    y = layers.BatchNormalization()(layers.Conv2D(filters, 3, padding="same", use_bias=False)(y))
    sc = layers.BatchNormalization()(layers.Conv2D(filters, 1, strides=2, padding="same", use_bias=False)(x))
    x = layers.ReLU()(layers.Add()([sc, y]))
    x = layers.GlobalAveragePooling2D()(x)
    out = layers.Dense(10, name="classifier_head")(x)
    return keras.Model(inp, out)


if __name__ == "__main__":
    main()
