#!/usr/bin/env bash
set -e

echo "=== TinyMLDelta CLI Installer ==="
echo "Creating virtual environment .tinyenv ..."

python3 -m venv .tinyenv
source .tinyenv/bin/activate

pip install --upgrade pip setuptools wheel

echo "Installing TinyMLDelta CLI requirements..."
pip install -r requirements.txt

echo ""
echo "======================================="
echo "TinyMLDelta CLI environment is ready!"
echo "To activate it:"
echo "    source .tinyenv/bin/activate"
echo ""
echo "Run patch generator with:"
echo "    python3 tinymldelta_patchgen.py base.tflite target.tflite patch.tmd"
echo "======================================="
