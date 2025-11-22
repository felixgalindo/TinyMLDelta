# TinyMLDelta

TinyMLDelta is an **incremental model update system** for TinyML and edge devices.

Instead of sending full models over-the-air, TinyMLDelta sends **small binary patches** that update only the changed bytes of your model (typically just weights). On device, a tiny C runtime applies the patch into an inactive slot, verifies guardrails, and atomically flips the active model.

- ✅ Designed for **TensorFlow Lite Micro (TFLM)** first  
- ✅ Weight-aware patching (diffs focus on constant tensors / weights)  
- ✅ Runtime-agnostic wire format (future ONNX / Edge Impulse support)  
- ✅ Guardrails: arena size, opset hash, I/O hash, ABI  

This demo bundle includes:

- A top-level `README.md` (this file)
- A model generator script (`examples/modelgen/make_models.py`)
- A POSIX flash verifier (`examples/posix/verify_flash.py`)
- A convenience script to run the full flow (`run_demo.sh`)

You can copy these files into your TinyMLDelta repo and follow the README instructions.
