#!/usr/bin/env bash
# deploy_service.sh — Build and deploy the TinyMLDelta + Edge Impulse demo app
#                     to the Arduino UNO Q Linux co-processor.
#
# demo_app_ei is a C++ binary that integrates:
#   TinyMLDelta C library  — tmd_apply_patch_from_memory() for OTA model updates
#   Edge Impulse SDK       — run_classifier() with external model loading
#
# The EI SDK bundles TFLite Micro — no separate shared library needed.
#
# Prerequisites:
#   The Edge Impulse SDK fork with external model loading support:
#     git clone -b feature/external-model-loading \
#       https://github.com/felixgalindo/edge-impulse-sdk-pack.git
#     export EI_SDK_LOCAL=/path/to/edge-impulse-sdk-pack
#
#   The fork adds EI_CLASSIFIER_EXTERNAL_MODEL_LOADING to the EI SDK,
#   enabling runtime model updates via TinyMLDelta without reflash.
#
# Usage:
#   EI_SDK_LOCAL=/path/to/sdk ./deploy_service.sh   # build + deploy + start
#   ./deploy_service.sh --stop                      # stop the service
#   ./deploy_service.sh --logs                      # tail service log
#   ./deploy_service.sh --restart                   # stop + start
#   ./deploy_service.sh --patch patch.tmd           # push a patch for 'u' cmd
#
# Author:  Felix Galindo
# License: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
COMMON_DIR="$DEMO_DIR/common"
EI_SDK_LOCAL="${EI_SDK_LOCAL:-$HOME/Projects/edge-impulse-sdk-pack}"
ADB="$HOME/Library/Arduino15/packages/arduino/tools/adb/32.0.0/adb"
REMOTE_DIR="/home/arduino/tinymldelta-ei"
SERVICE_BIN="demo_app_ei"
LOG_FILE="$REMOTE_DIR/demo_app_ei.log"

HR="────────────────────────────────────────────────────────────"
info() { echo "  [INFO]  $*"; }
ok()   { echo "  [ OK ]  $*"; }
warn() { echo "  [WARN]  $*"; }
die()  { echo "  [ERR]   $*" >&2; exit 1; }

# ── Check ADB ─────────────────────────────────────────────────────────────────
[ -x "$ADB" ] || die "ADB not found at $ADB"
$ADB devices | grep -q "device$" || die "No ADB device connected. Is the board plugged in?"

# ── Parse args ────────────────────────────────────────────────────────────────
ACTION="deploy"
PATCH_FILE=""
for arg in "$@"; do
    case $arg in
        --stop)    ACTION="stop" ;;
        --logs)    ACTION="logs" ;;
        --restart) ACTION="restart" ;;
        --patch)   ACTION="patch" ;;
        -h|--help)
            echo "Usage: $0 [--stop|--logs|--restart|--patch <file.tmd>]"
            exit 0 ;;
        *.tmd)
            PATCH_FILE="$arg" ;;
    esac
done

# ── Stop ──────────────────────────────────────────────────────────────────────
if [[ "$ACTION" == "stop" ]]; then
    info "Stopping demo_app_ei..."
    $ADB shell "pkill -x demo_app_ei 2>/dev/null; echo stopped" 2>&1
    ok "Service stopped."
    exit 0
fi

# ── Logs ──────────────────────────────────────────────────────────────────────
if [[ "$ACTION" == "logs" ]]; then
    info "Tailing demo_app_ei log (Ctrl-C to stop)..."
    $ADB shell "tail -f $LOG_FILE 2>/dev/null || echo 'No log yet'"
    exit 0
fi

# ── Push patch ────────────────────────────────────────────────────────────────
if [[ "$ACTION" == "patch" ]]; then
    [[ -n "$PATCH_FILE" ]] || die "Specify a .tmd file: $0 --patch patch.tmd"
    [[ -f "$PATCH_FILE" ]] || die "File not found: $PATCH_FILE"
    info "Pushing patch to board as pending_patch.tmd..."
    $ADB push "$PATCH_FILE" "$REMOTE_DIR/pending_patch.tmd"
    ok "Patch ready. Send 'u' in the Serial Monitor to apply."
    exit 0
fi

# ── Restart ───────────────────────────────────────────────────────────────────
if [[ "$ACTION" == "restart" ]]; then
    info "Restarting demo_app_ei..."
    $ADB shell "pkill -x demo_app_ei 2>/dev/null; true"
    sleep 1
    $ADB shell "cd $REMOTE_DIR && nohup ./$SERVICE_BIN > $LOG_FILE 2>&1 &
        sleep 2
        pgrep -x demo_app_ei > /dev/null && echo 'Restarted.' || \
        { echo 'ERROR: failed to restart'; tail -5 $LOG_FILE 2>/dev/null; }"
    exit 0
fi

# ── Deploy ────────────────────────────────────────────────────────────────────
echo "$HR"
echo "  TinyMLDelta + Edge Impulse — Build & Deploy to Arduino UNO Q"
echo "$HR"

# Check EI SDK exists locally
[[ -d "$EI_SDK_LOCAL/edgeimpulse/edge-impulse-sdk" ]] || \
    die "EI SDK not found at $EI_SDK_LOCAL. Set EI_SDK_LOCAL=path/to/edge-impulse-sdk-pack"

# Stop any running instance
info "Stopping any running instance..."
$ADB shell "pkill -x demo_app_ei 2>/dev/null; true"

# Create remote directory
info "Creating $REMOTE_DIR on board..."
$ADB shell "mkdir -p $REMOTE_DIR"

# ── Push sources ─────────────────────────────────────────────────────────────
info "Pushing source files to board..."

# Demo app + Makefile
$ADB push "$SCRIPT_DIR/demo_app_ei.cpp" "$REMOTE_DIR/demo_app_ei.cpp"
$ADB push "$SCRIPT_DIR/Makefile"        "$REMOTE_DIR/Makefile"

# Shared headers
$ADB shell "mkdir -p $REMOTE_DIR/common"
$ADB push "$COMMON_DIR/msgpack.h"         "$REMOTE_DIR/common/msgpack.h"
$ADB push "$COMMON_DIR/router_client.h"   "$REMOTE_DIR/common/router_client.h"
$ADB push "$COMMON_DIR/tmd_port_memory.h" "$REMOTE_DIR/common/tmd_port_memory.h"

# Model parameters + compiled TFLite model from EI export
$ADB shell "mkdir -p $REMOTE_DIR/model-parameters $REMOTE_DIR/tflite-model"
$ADB push "$SCRIPT_DIR/model-parameters/" "$REMOTE_DIR/model-parameters/"
$ADB push "$SCRIPT_DIR/tflite-model/"     "$REMOTE_DIR/tflite-model/"

# TinyMLDelta runtime
$ADB shell "mkdir -p $REMOTE_DIR/runtime/src $REMOTE_DIR/runtime/include"
$ADB push "$REPO_ROOT/runtime/src/tinymldelta_core.c"        "$REMOTE_DIR/runtime/src/tinymldelta_core.c"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta.h"         "$REMOTE_DIR/runtime/include/tinymldelta.h"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta_ports.h"   "$REMOTE_DIR/runtime/include/tinymldelta_ports.h"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta_config.h"  "$REMOTE_DIR/runtime/include/tinymldelta_config.h"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta_internal.h" "$REMOTE_DIR/runtime/include/tinymldelta_internal.h"

# Edge Impulse SDK (the fork with external model loading)
info "Pushing Edge Impulse SDK to board (this may take a minute)..."
$ADB shell "mkdir -p $REMOTE_DIR/edge-impulse-sdk-pack/edgeimpulse"
$ADB push "$EI_SDK_LOCAL/edgeimpulse/edge-impulse-sdk/" \
    "$REMOTE_DIR/edge-impulse-sdk-pack/edgeimpulse/edge-impulse-sdk/"

# ── Build on board ───────────────────────────────────────────────────────────
info "Building demo_app_ei on board (this may take several minutes)..."
$ADB shell "cd $REMOTE_DIR && make clean && \
    make -j4 \
        RUNTIME=./runtime \
        COMMON=./common \
        EI_SDK=./edge-impulse-sdk-pack \
    2>&1" && ok "Build successful." \
    || die "Build failed. Check logs above."

# ── Start demo_app_ei ────────────────────────────────────────────────────────
info "Starting demo_app_ei..."
$ADB shell "cd $REMOTE_DIR && nohup ./$SERVICE_BIN > $LOG_FILE 2>&1 &
    sleep 2
    if pgrep -x demo_app_ei > /dev/null; then
        echo 'Service started (PID:' \$(pgrep -x demo_app_ei) ')'
    else
        echo 'ERROR: Service failed to start.'
        tail -10 $LOG_FILE 2>/dev/null
    fi"

echo "$HR"
ok "demo_app_ei deployed and running on board Linux."
echo
echo "  Next steps:"
echo "    1. Flash the sketch (arduino/UnoQ_TinyMLDeltaDemo/)"
echo "    2. Open Serial Monitor — send 't' to collect training data"
echo "    3. Run: python3 make_model.py training_data.csv"
echo "    4. Run: $0 --patch patch.tmd   (pushes patch to board)"
echo "    5. In Serial Monitor: send 'u' to apply the patch"
echo
echo "  Useful commands:"
echo "    Logs:    $0 --logs"
echo "    Stop:    $0 --stop"
echo "    Restart: $0 --restart"
echo "    Patch:   $0 --patch patch.tmd"
echo "$HR"
