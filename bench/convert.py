#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
convert.py — Keras -> TFLite conversion (float32 and int8) for the benchmark.

int8 is what actually ships to MCUs, so it is the deployment-realistic
representation. float32 is included to expose how post-training quantization
amplifies byte-level deltas (the quantization-sensitivity effect).

Author:  Felix Galindo
License: Apache-2.0
"""

import numpy as np
import tensorflow as tf


def to_tflite_float32(model) -> bytes:
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    return conv.convert()


def to_tflite_int8(model, rep_data: np.ndarray, n: int = 200) -> bytes:
    """Full-integer int8 quantization using a representative dataset.

    rep_data: float32 samples in the model's input domain ([0,1] images).
    """
    samples = rep_data[:n]

    def rep_gen():
        for i in range(len(samples)):
            yield [samples[i:i + 1].astype("float32")]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_gen
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    return conv.convert()


def convert(model, quant: str, rep_data: np.ndarray) -> bytes:
    if quant == "float32":
        return to_tflite_float32(model)
    if quant == "int8":
        return to_tflite_int8(model, rep_data)
    raise ValueError(f"unknown quant mode: {quant}")
