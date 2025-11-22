#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TinyMLDelta — TFLite metadata extractor."""

from __future__ import annotations
import struct
import zlib
from typing import Tuple, Optional, List

_TFLITE_OK = False
try:
    import tflite  # type: ignore
    _TFLITE_OK = True
except Exception:
    try:
        from tflite_support import flatbuffers  # noqa: F401
        import tflite  # type: ignore
        _TFLITE_OK = True
    except Exception:
        _TFLITE_OK = False


def _mix32(x: int, y: int) -> int:
    h = (x ^ (y + 0x9E3779B9 + ((x << 6) & 0xFFFFFFFF) + ((x >> 2) & 0xFFFFFFFF))) & 0xFFFFFFFF
    return h


def _hash_list_u32(items: List[int]) -> int:
    if not items:
        return 0
    h = 0
    for v in sorted(items):
        h = _mix32(h, v & 0xFFFFFFFF)
    return h & 0xFFFFFFFF


def _dtype_size(tensor_type: int) -> int:
    table = {
        0: 4, 1: 2, 2: 4, 3: 1, 4: 8,
        6: 1, 7: 2, 8: 8, 9: 1, 10: 8,
        11: 16, 12: 8, 15: 4, 16: 2,
    }
    return table.get(tensor_type, 1)


def _tensor_bytes(shape_vec, ttype: int) -> int:
    if shape_vec is None:
        return 0
    numel = 1
    for i in range(shape_vec.Length()):
        dim = shape_vec.Get(i)
        if dim <= 0:
            dim = 1
        numel *= dim
    return numel * _dtype_size(ttype)


def _is_constant_tensor(model, subgraph, t_idx: int) -> bool:
    tensors = subgraph.Tensors()
    buf_idx = tensors[t_idx].Buffer()
    if buf_idx < 0:
        return False
    buf = model.Buffers(buf_idx)
    data = buf.DataAsNumpy()
    return data is not None and len(data) > 0


def _collect_model_info(model_buf: bytes, arena_factor: Optional[float]) -> Tuple[int, int, int, int]:
    if not _TFLITE_OK:
        raise RuntimeError("TFLite schema not available. Install: pip install flatbuffers tflite-support")

    model = tflite.Model.Model.GetRootAsModel(model_buf, 0)

    abi = int(model.Version())

    builtin_codes = []
    for i in range(model.OperatorCodesLength()):
        op_code = model.OperatorCodes(i)
        builtin = op_code.BuiltinCode()
        builtin_codes.append(int(builtin) & 0xFFFF)
    opset_hash = _hash_list_u32(builtin_codes)

    sg = model.Subgraphs(0)

    def _tensor_sig(t_idx: int):
        t = sg.Tensors(t_idx)
        sig = [int(t.Type())]
        shape = t.ShapeAsNumpy()
        if shape is not None:
            sig.extend([int(v) for v in shape.tolist()])
        return sig

    io_sig = []
    for i in range(sg.InputsLength()):
        io_sig.extend(_tensor_sig(sg.Inputs(i)))
    io_sig.append(0xDEADBEEF)
    for i in range(sg.OutputsLength()):
        io_sig.extend(_tensor_sig(sg.Outputs(i)))
    io_hash = zlib.crc32(struct.pack("<%di" % len(io_sig), *io_sig)) & 0xFFFFFFFF

    req_arena = 0
    if arena_factor and arena_factor > 0:
        total = 0
        for t_idx in range(sg.TensorsLength()):
            if _is_constant_tensor(model, sg, t_idx):
                continue
            tensor = sg.Tensors(t_idx)
            total += _tensor_bytes(tensor.Shape(), int(tensor.Type()))
        req_arena = int(total * float(arena_factor))

    return abi & 0xFFFF, opset_hash & 0xFFFFFFFF, io_hash & 0xFFFFFFFF, req_arena


def compute_from_tflite(path: str, arena_factor: Optional[float] = 1.4) -> Tuple[int, int, int, int]:
    with open(path, "rb") as f:
        buf = f.read()
    return _collect_model_info(buf, arena_factor)
