"""Model/format-agnostic test: patch + apply a non-TFLite model byte-exact.

TinyMLDelta deltas raw bytes, so it should work on any model artifact — not just
TFLite FlatBuffers. Here we build a real ONNX model (protobuf, a structurally very
different container), apply a weight update, and verify the patch reconstructs the
target ONNX bytes exactly. Skipped if `onnx` is not installed.
"""
import numpy as np
import pytest

from applier import apply_patch

onnx = pytest.importorskip("onnx")
from onnx import TensorProto, helper, numpy_helper  # noqa: E402


def _build_onnx_mlp(seed: int):
    """A small MLP: X(1,16) -> Gemm -> Relu -> Gemm -> Y(1,4), with weights."""
    rng = np.random.default_rng(seed)
    inits = [
        numpy_helper.from_array(rng.standard_normal((16, 8)).astype(np.float32), "W1"),
        numpy_helper.from_array(rng.standard_normal((8,)).astype(np.float32), "b1"),
        numpy_helper.from_array(rng.standard_normal((8, 4)).astype(np.float32), "W2"),
        numpy_helper.from_array(rng.standard_normal((4,)).astype(np.float32), "b2"),
    ]
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
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])


def _perturb_weight(model, name: str):
    for init in model.graph.initializer:
        if init.name == name:
            arr = numpy_helper.to_array(init).copy()
            arr.flat[0] += 0.5                      # change one weight
            init.CopyFrom(numpy_helper.from_array(arr, name))
            return
    raise AssertionError(f"initializer {name} not found")


def test_onnx_weight_update_roundtrip(make_patch):
    model = _build_onnx_mlp(seed=1)
    base = model.SerializeToString()
    _perturb_weight(model, "W2")
    target = model.SerializeToString()

    assert base != target
    assert len(base) == len(target)                 # same-size weight edit

    patch = make_patch(base, target)
    assert apply_patch(base, patch) == target       # byte-exact on ONNX/protobuf
    assert len(patch) < len(target)                 # delta smaller than the model


def test_onnx_growth_roundtrip(make_patch):
    """ONNX target that grows (extra initializer) still reconstructs byte-exact."""
    model = _build_onnx_mlp(seed=2)
    base = model.SerializeToString()
    model.graph.initializer.append(
        numpy_helper.from_array(np.arange(32, dtype=np.float32), "extra")
    )
    target = model.SerializeToString()

    assert len(target) > len(base)
    patch = make_patch(base, target)
    assert apply_patch(base, patch) == target
