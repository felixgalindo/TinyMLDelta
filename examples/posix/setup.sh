#!/usr/bin/env bash
# setup.sh — Install dependencies and build the TinyMLDelta POSIX demo.
#
# Usage:
#   ./setup.sh        # install Python deps + build demo_apply
#   ./setup.sh --run  # install + build + run the full demo
#
# Author:  Felix Galindo
# License: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUN_AFTER=false

for arg in "$@"; do
    case $arg in
        --run)  RUN_AFTER=true ;;
        -h|--help)
            echo "Usage: $0 [--run]"
            echo "  --run   build AND run the end-to-end demo"
            exit 0 ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

HR="────────────────────────────────────────────────────────────"
info()  { echo "  [INFO]  $*"; }
ok()    { echo "  [ OK ]  $*"; }
die()   { echo "  [ERR]   $*" >&2; exit 1; }

echo "$HR"
echo "  TinyMLDelta POSIX Demo — Setup"
echo "$HR"

# ── Python ────────────────────────────────────────────────────────────────────
info "Checking Python 3..."
command -v python3 &>/dev/null || die "python3 not found. Install from https://python.org"
PY_VER=$(python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
info "  Python $PY_VER"

info "Installing Python packages (tensorflow numpy)..."
python3 -m pip install --quiet --upgrade tensorflow numpy
ok "Python packages installed."

# ── C compiler ────────────────────────────────────────────────────────────────
info "Checking C compiler..."
if command -v clang &>/dev/null; then
    CC=clang; info "  clang found."
elif command -v gcc &>/dev/null; then
    CC=gcc; info "  gcc found."
else
    die "No C compiler found. Install Xcode Command Line Tools (macOS) or build-essential (Linux)."
fi

# ── Build demo_apply ──────────────────────────────────────────────────────────
echo "$HR"
info "Building demo_apply..."
cd "$SCRIPT_DIR"
make clean >/dev/null 2>&1 || true
make all
ok "demo_apply built."

echo "$HR"
ok "Setup complete."
echo
echo "  Run the demo:"
echo "    cd $SCRIPT_DIR"
echo "    ./run_demo.sh"
echo "$HR"

if $RUN_AFTER; then
    echo
    bash "${SCRIPT_DIR}/run_demo.sh"
fi
