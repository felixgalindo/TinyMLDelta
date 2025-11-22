#!/usr/bin/env bash
# TinyMLDelta end-to-end demo helper.
# Generates base/target models, builds a patch, applies it to a simulated
# flash image via the POSIX example, and verifies the result.
set -e

echo "[run_demo] Step 0: ensure we are at TinyMLDelta repo root"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

echo "[run_demo] Step 1: generate base.tflite and target.tflite"
cd examples/modelgen
python3 make_models.py
cd "$ROOT_DIR"

echo "[run_demo] Step 2: generate TinyMLDelta patch (patch.tmd)"
python3 cli/tinymldelta_patchgen.py \
    examples/modelgen/base.tflite \
    examples/modelgen/target.tflite \
    patch.tmd \
    --auto-meta

echo "[run_demo] Step 3: build POSIX demo"
cd examples/posix
make
cd "$ROOT_DIR"

echo "[run_demo] Step 4: create simulated flash image"
cd examples/posix
python3 make_flash.py --size 262144
cd "$ROOT_DIR"

echo "[run_demo] Step 5: apply patch to flash.bin"
cd examples/posix
./demo_apply flash.bin ../../patch.tmd
cd "$ROOT_DIR"

echo "[run_demo] Step 6: verify that active flash contains target model"
cd examples/posix
python3 verify_flash.py flash.bin ../../examples/modelgen/target.tflite
cd "$ROOT_DIR"

echo "[run_demo] Step 7: show patch vs full model size"
echo
ls -lh examples/modelgen/target.tflite patch.tmd || true

echo
echo "[run_demo] Done."
