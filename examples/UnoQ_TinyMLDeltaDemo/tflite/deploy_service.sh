#!/usr/bin/env bash
# deploy_service.sh — Build and deploy the TinyMLDelta demo app to the
#                     Arduino UNO Q Linux co-processor.
#
# demo_app is a C++ binary that directly integrates:
#   TinyMLDelta C library  — tmd_apply_patch_from_memory() for OTA model updates
#   TFLite C API           — autoencoder inference
#
# The STM32 sketch (UnoQ_TinyMLDeltaDemo.ino) is a thin sensor proxy that
# forwards temperature readings and Monitor keystrokes via Bridge.call().
#
# Usage:
#   ./deploy_service.sh                   # build + deploy + start demo_app
#   ./deploy_service.sh --stop            # stop the service
#   ./deploy_service.sh --logs            # tail service log
#   ./deploy_service.sh --restart         # stop + start
#   ./deploy_service.sh --patch patch.tmd # push a patch for the 'u' command
#
# After deploying, open the Arduino IDE Serial Monitor and send:
#   t — collect training data
#   i — start inference
#   u — apply a previously pushed patch
#   s — status
#   ? — help
#
# Author:  Felix Galindo
# License: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
COMMON_DIR="$DEMO_DIR/common"
ADB="$HOME/Library/Arduino15/packages/arduino/tools/adb/32.0.0/adb"
REMOTE_DIR="/home/arduino/tinymldelta"
SERVICE_BIN="demo_app"
LOG_FILE="$REMOTE_DIR/demo_app.log"

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
    info "Stopping demo_app..."
    $ADB shell "pkill -x demo_app 2>/dev/null; echo stopped" 2>&1
    ok "Service stopped."
    exit 0
fi

# ── Logs ──────────────────────────────────────────────────────────────────────
if [[ "$ACTION" == "logs" ]]; then
    info "Tailing demo_app log (Ctrl-C to stop)..."
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
    info "Restarting demo_app..."
    $ADB shell "pkill -x demo_app 2>/dev/null; true"
    sleep 1
    $ADB shell "cd $REMOTE_DIR && nohup ./$SERVICE_BIN > $LOG_FILE 2>&1 &
        sleep 2
        pgrep -x demo_app > /dev/null && echo 'Restarted.' || \
        { echo 'ERROR: failed to restart'; tail -5 $LOG_FILE 2>/dev/null; }"
    exit 0
fi

# ── Deploy ────────────────────────────────────────────────────────────────────
echo "$HR"
echo "  TinyMLDelta — Build & Deploy demo_app to Arduino UNO Q Linux"
echo "$HR"

# Stop any running instance
info "Stopping any running instance..."
$ADB shell "pkill -x demo_app 2>/dev/null; true"

# Create remote directory
info "Creating $REMOTE_DIR on board..."
$ADB shell "mkdir -p $REMOTE_DIR"

# ── Check / push TFLite C library ─────────────────────────────────────────────
TFLITE_SO="libtensorflowlite_c.so"
TFLITE_VERSION="2.17.1"
TFLITE_URL="https://github.com/tphakala/tflite_c/releases/download/v${TFLITE_VERSION}/tflite_c_v${TFLITE_VERSION}_linux_arm64.tar.gz"

info "Checking TFLite C library on board..."
HAS_TFLITE=$($ADB shell "[ -f $REMOTE_DIR/$TFLITE_SO ] && echo yes || echo no" 2>&1)
if [[ "$HAS_TFLITE" == *"no"* ]]; then
    TFLITE_LOCAL="$SCRIPT_DIR/$TFLITE_SO"
    if [[ ! -f "$TFLITE_LOCAL" ]]; then
        info "  Downloading $TFLITE_SO v$TFLITE_VERSION for aarch64..."
        TFLITE_TMP=$(mktemp -d)
        if curl -sL "$TFLITE_URL" | tar xz -C "$TFLITE_TMP" 2>/dev/null; then
            # The tarball contains libtensorflowlite_c.so.X.Y.Z
            TFLITE_EXTRACTED=$(find "$TFLITE_TMP" -name "libtensorflowlite_c*" -type f | head -1)
            if [[ -n "$TFLITE_EXTRACTED" ]]; then
                cp "$TFLITE_EXTRACTED" "$TFLITE_LOCAL"
                ok "  Downloaded $TFLITE_SO ($TFLITE_VERSION)"
            fi
        else
            warn "  Download failed. Check internet connection."
        fi
        rm -rf "$TFLITE_TMP"
    fi
    if [[ -f "$TFLITE_LOCAL" ]]; then
        info "  Pushing $TFLITE_SO to board..."
        $ADB push "$TFLITE_LOCAL" "$REMOTE_DIR/$TFLITE_SO"
    else
        die "$TFLITE_SO not available. Cannot build demo_app."
    fi
else
    ok "  $TFLITE_SO already on board."
fi

# ── Push sources and build on board ───────────────────────────────────────────
info "Pushing source files to board..."

# Push demo_app sources and Makefile
$ADB push "$SCRIPT_DIR/demo_app.cpp"       "$REMOTE_DIR/demo_app.cpp"
$ADB push "$COMMON_DIR/msgpack.h"         "$REMOTE_DIR/msgpack.h"
$ADB push "$COMMON_DIR/router_client.h"   "$REMOTE_DIR/router_client.h"
$ADB push "$COMMON_DIR/tmd_port_memory.h" "$REMOTE_DIR/tmd_port_memory.h"
$ADB push "$SCRIPT_DIR/Makefile"           "$REMOTE_DIR/Makefile"

# Push TinyMLDelta runtime sources (needed by Makefile)
$ADB shell "mkdir -p $REMOTE_DIR/runtime/src $REMOTE_DIR/runtime/include"
$ADB push "$REPO_ROOT/runtime/src/tinymldelta_core.c"    "$REMOTE_DIR/runtime/src/tinymldelta_core.c"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta.h"     "$REMOTE_DIR/runtime/include/tinymldelta.h"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta_ports.h" "$REMOTE_DIR/runtime/include/tinymldelta_ports.h"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta_config.h" "$REMOTE_DIR/runtime/include/tinymldelta_config.h"
$ADB push "$REPO_ROOT/runtime/include/tinymldelta_internal.h" "$REMOTE_DIR/runtime/include/tinymldelta_internal.h"

info "Building demo_app on board (this may take ~30 s)..."
$ADB shell "cd $REMOTE_DIR && make RUNTIME=./runtime TFLITE_LIB=$REMOTE_DIR 2>&1" && ok "Build successful." \
    || die "Build failed. Check board logs above."

# ── Push initial model if available ───────────────────────────────────────────
if [[ -f "$SCRIPT_DIR/base.tflite" ]]; then
    info "Pushing base.tflite as initial model..."
    $ADB push "$SCRIPT_DIR/base.tflite" "$REMOTE_DIR/model.tflite"
    ok "Model deployed."
else
    info "No base.tflite found — model will be deployed after training."
    info "Run: python3 make_model.py training_data.csv"
fi

# ── Start demo_app ────────────────────────────────────────────────────────────
info "Starting demo_app..."
$ADB shell "cd $REMOTE_DIR && nohup ./$SERVICE_BIN > $LOG_FILE 2>&1 &
    sleep 2
    if pgrep -x demo_app > /dev/null; then
        echo 'Service started (PID:' \$(pgrep -x demo_app) ')'
    else
        echo 'ERROR: Service failed to start.'
        tail -10 $LOG_FILE 2>/dev/null
    fi"

echo "$HR"
ok "demo_app deployed and running on board Linux."
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
