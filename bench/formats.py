#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
formats.py — Cross-format patch-size comparison.

The SAME logical model and the SAME logical update, serialized in different model
formats, to show two things:
  1. TinyMLDelta is format-agnostic (patches every format byte-exact).
  2. Byte-diff behaviour depends on the serialization container's offset
     stability — protobuf (ONNX) vs FlatBuffer (TFLite) vs contiguous tensors
     (GGUF / raw-weights class) differ for the same logical change.

Formats:
  flat    contiguous raw float32 tensors (GGUF / CMSIS-NN / raw-weights class)
  onnx    protobuf (ONNX)              — needs `onnx`
  tflite  FlatBuffer (TFLite)          — with --tflite (needs tensorflow)

Updates:
  weight  perturb one weight tensor (same-size edit)
  grow    append a tensor (model growth)

Usage:
  python3 formats.py                 # flat + onnx
  python3 formats.py --tflite        # also tflite (slower)

License: Apache-2.0
"""

import argparse
import os
import struct
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
from patchsize import measure          # noqa: E402
from backends import measure_backends  # noqa: E402


# --------------------------------------------------------------------------- #
#  Canonical weights + the two logical updates (shared across all formats)     #
# --------------------------------------------------------------------------- #

def base_weights(seed: int = 1):
    rng = np.random.default_rng(seed)
    return {
        "W1": rng.standard_normal((16, 8)).astype(np.float32),
        "b1": rng.standard_normal((8,)).astype(np.float32),
        "W2": rng.standard_normal((8, 4)).astype(np.float32),
        "b2": rng.standard_normal((4,)).astype(np.float32),
    }


def update_weight(w):
    w2 = {k: v.copy() for k, v in w.items()}
    w2["W2"].flat[0] += 0.5                 # one weight changes (same size)
    return w2


def update_grow(w):
    w2 = {k: v.copy() for k, v in w.items()}
    w2["extra"] = np.arange(64, dtype=np.float32)   # model grows
    return w2


# --------------------------------------------------------------------------- #
#  Serializers                                                                 #
# --------------------------------------------------------------------------- #

def to_flat(w) -> bytes:
    """A simple contiguous-tensor container (GGUF / raw-weights class)."""
    out = bytearray(b"FLAT")
    out += struct.pack("<I", len(w))
    for name, arr in w.items():
        nb = name.encode()
        out += struct.pack("<H", len(nb)) + nb
        out += struct.pack("<B", arr.ndim)
        out += b"".join(struct.pack("<I", d) for d in arr.shape)
        out += arr.astype("<f4").tobytes()
    return bytes(out)


def to_onnx(w) -> bytes:
    import onnx
    from onnx import TensorProto, helper, numpy_helper
    inits = [numpy_helper.from_array(v, k) for k, v in w.items()]
    nodes = [
        helper.make_node("Gemm", ["X", "W1", "b1"], ["h0"]),
        helper.make_node("Relu", ["h0"], ["h1"]),
        helper.make_node("Gemm", ["h1", "W2", "b2"], ["Y"]),
    ]
    graph = helper.make_graph(
        nodes, "mlp",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, [1, 16])],
        [helper.make_tensor_value_info("Y", TensorProto.FLOAT, [1, 4])],
        inits,
    )
    return helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)]
    ).SerializeToString()


def to_tflite(w) -> bytes:
    import tensorflow as tf
    inp = tf.keras.Input(shape=(16,))
    x = tf.keras.layers.Dense(8, activation="relu")(inp)
    out = tf.keras.layers.Dense(4)(x)
    m = tf.keras.Model(inp, out)
    m.set_weights([w["W1"], w["b1"], w["W2"], w["b2"]])
    return tf.lite.TFLiteConverter.from_keras_model(m).convert()


# --------------------------------------------------------------------------- #

def run(fmt_name, serialize, updates):
    base_w = base_weights()
    base = serialize(base_w)
    rows = []
    for up_name, up in updates:
        target = serialize(up(base_w))
        m = measure(base, target)
        bk = measure_backends(base, target)
        rows.append((fmt_name, up_name, len(base), len(target),
                     m["tmd_bytes"], m["tmd_ratio"], bk["rle"]["bytes"],
                     bk["lz4"]["bytes"]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tflite", action="store_true", help="also test TFLite (needs TF)")
    args = ap.parse_args()

    formats = [("flat", to_flat)]
    try:
        import onnx  # noqa: F401
        formats.append(("onnx", to_onnx))
    except Exception:
        print("[skip] onnx not installed")
    if args.tflite:
        formats.append(("tflite", to_tflite))

    rows = []
    for name, ser in formats:
        # tflite has no easy 'grow' analogue; weight update only there
        ups = [("weight", update_weight)]
        if name != "tflite":
            ups.append(("grow", update_grow))
        rows += run(name, ser, ups)

    print(f"\n  {'format':8}{'update':8}{'base':>8}{'target':>8}"
          f"{'tmd':>8}{'ratio':>8}{'rle':>7}{'lz4':>7}")
    for (f, u, b, t, tmd, ratio, rle, lz4) in rows:
        rt = f"{ratio*100:.2f}%" if ratio else "—"
        print(f"  {f:8}{u:8}{b:>8}{t:>8}{tmd:>8}{rt:>8}"
              f"{(rle if rle is not None else '—'):>7}"
              f"{(lz4 if lz4 is not None else '—'):>7}")


if __name__ == "__main__":
    main()
