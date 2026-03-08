#!/usr/bin/env bash
# setup.sh — Install dependencies and compile (optionally upload) the
#             TinyMLDelta Arduino UNO Q demo sketch.
#
# Usage:
#   ./setup.sh           # install deps + compile
#   ./setup.sh --upload  # install deps + compile + upload
#
# Requirements:
#   - Python 3.9+
#   - arduino-cli 1.4+ (https://arduino.github.io/arduino-cli/latest/installation/)
#
# Author:  Felix Galindo
# License: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPLOAD=false

# ── Parse args ────────────────────────────────────────────────────────────────
for arg in "$@"; do
    case $arg in
        --upload) UPLOAD=true ;;
        -h|--help)
            echo "Usage: $0 [--upload]"
            echo "  --upload   compile AND upload to a connected Arduino UNO Q"
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

info "Installing Python packages (tensorflow numpy pyserial)..."
python3 -m pip install --quiet --upgrade tensorflow numpy pyserial
ok "Python packages installed."

# ── 2. arduino-cli ────────────────────────────────────────────────────────────
info "Checking arduino-cli..."
if ! command -v arduino-cli &>/dev/null; then
    warn "arduino-cli not found."
    echo
    echo "  Install it with one of:"
    echo "    macOS:  brew install arduino-cli"
    echo "    Linux:  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh"
    echo "    Or see: https://arduino.github.io/arduino-cli/latest/installation/"
    echo
    die "arduino-cli required to compile/upload the sketch."
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

# ── 4. Arduino libraries ──────────────────────────────────────────────────────
info "Checking required Arduino libraries..."

install_lib_if_missing() {
    local lib="$1"
    if arduino-cli lib list 2>/dev/null | grep -q "$(echo "$lib" | cut -d' ' -f1)"; then
        ok "  '$lib' already installed."
    else
        info "  Installing '$lib'..."
        arduino-cli lib install "$lib"
        ok "  '$lib' installed."
    fi
}

install_lib_if_missing "SparkFun MCP9600 Thermocouple Library"
install_lib_if_missing "MsgPack"

# ── 5. Compile ────────────────────────────────────────────────────────────────
hr
info "Compiling sketch (TMD_HAS_TFLM=0 — Z-score + patch demo)..."
arduino-cli compile \
    --fqbn arduino:zephyr:unoq \
    --build-property "build.extra_flags=-DTMD_HAS_TFLM=0" \
    "$SCRIPT_DIR"

ok "Compile successful."

# ── 6. Upload (optional) ─────────────────────────────────────────────────────
if $UPLOAD; then
    hr
    info "Detecting Arduino UNO Q port..."
    PORT=$(arduino-cli board list 2>/dev/null \
        | awk '/Arduino UNO Q/{print $1}' | head -1)

    if [[ -z "$PORT" ]]; then
        warn "No Arduino UNO Q detected. Is the board connected?"
        warn "Try running manually:"
        warn "  arduino-cli upload --fqbn arduino:zephyr:unoq --port <port> $SCRIPT_DIR"
        exit 1
    fi

    info "  Uploading to $PORT ..."
    arduino-cli upload \
        --fqbn arduino:zephyr:unoq \
        --port "$PORT" \
        "$SCRIPT_DIR"
    ok "Upload complete."
fi

# ── Done ──────────────────────────────────────────────────────────────────────
hr
ok "Setup complete."
echo
echo "  Next steps:"
echo "    1. Flash the sketch (if not done yet):"
echo "       $0 --upload"
echo
echo "    2. Run the interactive demo:"
echo "       python3 $SCRIPT_DIR/run_demo.py"
echo
echo "    Or follow the manual steps in README.md."
hr
