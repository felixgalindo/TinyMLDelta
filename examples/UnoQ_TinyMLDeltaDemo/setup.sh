#!/usr/bin/env bash
# setup.sh — One-shot setup for the TinyMLDelta Arduino UNO Q demo.
#
# Does everything in order:
#   1. Install Python packages (tensorflow, numpy)
#   2. Install arduino-cli platform
#   3. Deploy demo_app to board Linux side (via ADB)
#   4. Compile + upload the sketch
#
# Usage:
#   ./setup.sh              # full setup (board must be connected)
#   ./setup.sh --no-upload  # skip sketch upload (compile only)
#
# Requirements:
#   - Python 3.9+
#   - arduino-cli  (https://arduino.github.io/arduino-cli/latest/installation/)
#   - Arduino UNO Q connected via USB
#
# Author:  Felix Galindo
# License: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPLOAD=true

# ── Parse args ────────────────────────────────────────────────────────────────
for arg in "$@"; do
    case $arg in
        --no-upload) UPLOAD=false ;;
        -h|--help)
            echo "Usage: $0 [--no-upload]"
            echo "  --no-upload   compile but do not upload to the board"
            exit 0 ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# ── Helpers ───────────────────────────────────────────────────────────────────
info()  { echo "  [INFO]  $*"; }
ok()    { echo "  [ OK ]  $*"; }
warn()  { echo "  [WARN]  $*"; }
die()   { echo "  [ERR]   $*" >&2; exit 1; }
hr()    { echo "────────────────────────────────────────────────────────────"; }

hr
echo "  TinyMLDelta UNO Q Demo — Setup"
hr

# ── 1. Python dependencies ────────────────────────────────────────────────────
info "Checking Python..."
if ! command -v python3 &>/dev/null; then
    die "python3 not found. Install Python 3.9+ from https://python.org"
fi
PY_VER=$(python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
info "  Python $PY_VER"

info "Installing Python packages (tensorflow numpy)..."
python3 -m pip install --quiet --upgrade tensorflow numpy
ok "Python packages installed."

# ── 2. arduino-cli ────────────────────────────────────────────────────────────
info "Checking arduino-cli..."
if ! command -v arduino-cli &>/dev/null; then
    echo
    echo "  arduino-cli not found. Install it with one of:"
    echo "    macOS:  brew install arduino-cli"
    echo "    Linux:  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh"
    echo "    Docs:   https://arduino.github.io/arduino-cli/latest/installation/"
    echo
    die "arduino-cli required."
fi
CLI_VER=$(arduino-cli version | awk '{print $3}')
info "  arduino-cli $CLI_VER"

# ── 3. arduino:zephyr platform ───────────────────────────────────────────────
info "Checking arduino:zephyr platform..."
if ! arduino-cli core list 2>/dev/null | grep -q "arduino:zephyr"; then
    info "  Installing arduino:zephyr platform..."
    arduino-cli core update-index
    arduino-cli core install arduino:zephyr
fi
ZEP_VER=$(arduino-cli core list 2>/dev/null | awk '/arduino:zephyr/{print $2}')
ok "  arduino:zephyr $ZEP_VER"

# ── 4. Deploy Linux demo app ────────────────────────────────────────────────
hr
info "Deploying demo_app to board Linux side via ADB..."
ADB="$HOME/Library/Arduino15/packages/arduino/tools/adb/32.0.0/adb"
if [ -x "$ADB" ] && $ADB devices 2>/dev/null | grep -q "device$"; then
    bash "$SCRIPT_DIR/linux/deploy_service.sh"
    ok "demo_app deployed."
else
    warn "Board not found via ADB — skipping Linux deploy."
    warn "Run './linux/deploy_service.sh' once the board is connected."
fi

# ── 5. Compile ────────────────────────────────────────────────────────────────
hr
info "Compiling sketch..."
SKETCH_DIR="$SCRIPT_DIR/arduino/UnoQ_TinyMLDeltaDemo"
arduino-cli compile --fqbn arduino:zephyr:unoq "$SKETCH_DIR"
ok "Compile successful."

# ── 6. Upload ────────────────────────────────────────────────────────────────
if $UPLOAD; then
    hr
    info "Detecting Arduino UNO Q..."
    PORT=$(arduino-cli board list 2>/dev/null \
        | awk '/Arduino UNO Q/{print $1}' | head -1)

    if [[ -z "$PORT" ]]; then
        warn "Arduino UNO Q not detected on any port."
        warn "Connect the board and run:"
        warn "  arduino-cli upload --fqbn arduino:zephyr:unoq --port <port> $SKETCH_DIR"
    else
        info "  Uploading to $PORT ..."
        arduino-cli upload --fqbn arduino:zephyr:unoq --port "$PORT" "$SKETCH_DIR"
        ok "Sketch uploaded."
    fi
fi

# ── Done ──────────────────────────────────────────────────────────────────────
hr
ok "Setup complete."
echo
echo "  Run the demo:"
echo "    python3 $SCRIPT_DIR/run_demo.py"
echo
hr
