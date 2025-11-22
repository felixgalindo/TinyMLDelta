#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# @file run_demo.sh
# @brief TinyMLDelta POSIX end-to-end demo driver for macOS/Linux.
# @author Felix Galindo
# @license Apache-2.0
#
# Steps:
#   1) Generate base.tflite and target.tflite using examples/modelgen/make_models.py
#   2) Run CLI patch generator to produce examples/posix/patch.tmd
#   3) Build POSIX demo (demo_apply)
#   4) Create a simulated flash.bin with two model slots (A/B)
#   5) Apply TinyMLDelta patch to flash.bin (via demo_apply)
#   6) Verify that target.tflite bytes appear in flash.bin
#-----------------------------------------------------------------------------

set -euo pipefail

# Resolve repo root based on this script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}"

echo "[run_demo] Repo root       = ${REPO_ROOT}"

MODEL_DIR="${REPO_ROOT}/examples/modelgen"
POSIX_DIR="${REPO_ROOT}/examples/posix"
PATCHGEN_PY="${REPO_ROOT}/cli/tinymldelta_patchgen.py"
VERIFY_FLASH_PY="${POSIX_DIR}/verify_flash.py"

BASE_TFLITE="${MODEL_DIR}/base.tflite"
TARGET_TFLITE="${MODEL_DIR}/target.tflite"
PATCH_FILE="${POSIX_DIR}/patch.tmd"
FLASH_BIN="${POSIX_DIR}/flash.bin"

echo "[run_demo] Model dir       = ${MODEL_DIR}"
echo "[run_demo] POSIX demo dir  = ${POSIX_DIR}"

#-----------------------------------------------------------------------------
# Step 1: Generate base.tflite and target.tflite
#-----------------------------------------------------------------------------
echo "[run_demo] Step 1: generate base.tflite and target.tflite"

cd "${MODEL_DIR}"
python3 make_models.py

#-----------------------------------------------------------------------------
# Step 2: Generate TinyMLDelta patch (patch.tmd)
#-----------------------------------------------------------------------------
echo "[run_demo] Step 2: generate TinyMLDelta patch (patch.tmd)"

cd "${REPO_ROOT}"

python3 "${PATCHGEN_PY}" \
  "${BASE_TFLITE}" \
  "${TARGET_TFLITE}" \
  "${PATCH_FILE}"

PATCH_SIZE=$(stat -f "%z" "${PATCH_FILE}" 2>/dev/null || stat -c "%s" "${PATCH_FILE}")
BASE_SIZE=$(stat -f "%z" "${BASE_TFLITE}" 2>/dev/null || stat -c "%s" "${BASE_TFLITE}")
TARGET_SIZE=$(stat -f "%z" "${TARGET_TFLITE}" 2>/dev/null || stat -c "%s" "${TARGET_TFLITE}")

echo "[run_demo] Patch path       : ${PATCH_FILE}"
echo "[run_demo] Patch size       : $(printf "%8d" "${PATCH_SIZE}") bytes"
echo "[run_demo] Base model size  : $(printf "%8d" "${BASE_SIZE}") bytes"
echo "[run_demo] Target model size: $(printf "%8d" "${TARGET_SIZE}") bytes"

#-----------------------------------------------------------------------------
# Step 3: Build POSIX demo
#-----------------------------------------------------------------------------
echo "[run_demo] Step 3: build POSIX demo"

cd "${POSIX_DIR}"
make clean >/dev/null 2>&1 || true
make all

#-----------------------------------------------------------------------------
# Step 4: Create simulated flash image (flash.bin)
#-----------------------------------------------------------------------------
echo "[run_demo] Step 4: create simulated flash image"

python3 make_flash.py \
    --flash "${FLASH_BIN}" \
    --base "${BASE_TFLITE}"

FLASH_SIZE=$(stat -f "%z" "${FLASH_BIN}" 2>/dev/null || stat -c "%s" "${FLASH_BIN}")
echo "[run_demo] flash.bin size   : $(printf "%8d" "${FLASH_SIZE}") bytes"

#-----------------------------------------------------------------------------
# Step 5: Apply patch to flash.bin using demo_apply
#-----------------------------------------------------------------------------
echo "[run_demo] Step 5: apply patch to flash.bin"

cd "${POSIX_DIR}"
./demo_apply "${FLASH_BIN}" "${PATCH_FILE}"

#-----------------------------------------------------------------------------
# Step 6: Verify that flash contains target model bytes
#-----------------------------------------------------------------------------
echo "[run_demo] Step 6: verify flash contents match target model"

python3 "${VERIFY_FLASH_PY}" \
    --flash "${FLASH_BIN}" \
    --target "${TARGET_TFLITE}" \
    --patch "${PATCH_FILE}"
