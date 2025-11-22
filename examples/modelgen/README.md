# TinyMLDelta Model Generator Demo

This folder contains a small helper script that generates two simple  
TensorFlow Lite models:

- `base.tflite` – initial version  
- `target.tflite` – same architecture, slightly different weights  

This lets you test TinyMLDelta *end-to-end* without writing ML code.

## Requirements

Create (or reuse) a Python environment and install dependencies:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install "tensorflow>=2.10"
pip install -r cli/requirements.txt  # if you have the TinyMLDelta repo
```

At minimum, you need TensorFlow.

## Generate the models

From the TinyMLDelta repo root:

```bash
cd examples/modelgen
python3 make_models.py
cd ../..
```

You should see output similar to:

```text
[ModelGen] Saved base.tflite (XXXX bytes)
[ModelGen] Saved target.tflite (XXXX bytes)
[ModelGen] Done.
```

The generated models live in `examples/modelgen/`.
