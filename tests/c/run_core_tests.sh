#!/usr/bin/env bash
# Integration test: drive the real TinyMLDelta C core over several base->target
# cases (TF-free). For each case: generate blobs -> PatchGen -> apply via the
# core harness -> assert the result equals the target byte-for-byte.
#
# Honors CXX / CC / CXXFLAGS so CI can build with sanitizers.
# License: Apache-2.0
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PY="${PY:-python3}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
CXXFLAGS="${CXXFLAGS:--std=c++11 -O1 -g}"
CFLAGS="${CFLAGS:--std=c99 -O1 -g}"
SLOT=262144

# Persistent build dir (BUILD_DIR) keeps object + gcov data for the coverage job;
# otherwise use a temp dir that is cleaned on exit.
if [ -n "${BUILD_DIR:-}" ]; then
  WORK="$BUILD_DIR"; mkdir -p "$WORK"
else
  WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fi

INC="-I$ROOT/runtime/include -I$ROOT/examples/UnoQ_TinyMLDeltaDemo/common"

echo "[build] compiling core + harness"
$CC $CFLAGS -DTMD_FEAT_LZ4TINY=1 -DTMD_FEAT_COPYADD=1 $INC -c "$ROOT/runtime/src/tinymldelta_core.c" -o "$WORK/core.o"
$CXX $CXXFLAGS $INC -c "$HERE/core_apply_harness.cpp" -o "$WORK/harness.o"
$CXX $CXXFLAGS "$WORK/core.o" "$WORK/harness.o" -o "$WORK/core_apply"

gen() {  # gen <kind> <base.bin> <target.bin>
  "$PY" - "$1" "$2" "$3" <<'PY'
import sys, random
kind, bp, tp = sys.argv[1], sys.argv[2], sys.argv[3]
rng = random.Random(hash(kind) & 0xffff)
def rnd(n,s): r=random.Random(s); return bytes(r.randrange(256) for _ in range(n))
if kind == "identical":
    base = rnd(4000, 1); target = base
elif kind == "scattered":
    base = bytearray(rnd(8000, 2)); target = bytearray(base)
    for _ in range(150): target[rng.randrange(len(target))] ^= 0xFF
    base, target = bytes(base), bytes(target)
elif kind == "growth":
    base = rnd(4000, 3); target = base + rnd(1200, 4)
elif kind == "shrink":
    base = rnd(4000, 5); target = base[:2500]
elif kind == "rle":
    base = rnd(6000, 6); target = bytearray(base); target[1000:3000] = b"\x00"*2000
    base, target = bytes(base), bytes(target)
elif kind == "large":
    base = bytes(200000); target = bytearray(base)
    for i in range(5000, 180000): target[i] = 0xAB
    base, target = bytes(base), bytes(target)
elif kind == "lz4":
    base = rnd(6000, 7); target = bytearray(base)
    target[1000:4500] = b"\x00"*3500
    target[5000:5800] = (b"ABCD"*200)[:800]
    base, target = bytes(base), bytes(target)
elif kind == "copyadd_basic":
    base = bytearray(rnd(8000, 8)); target = bytearray(base)
    r2 = random.Random(15)
    for _ in range(60): target[r2.randrange(len(target))] ^= 0xFF
    base, target = bytes(base), bytes(target)
elif kind == "copyadd_shift":
    base = rnd(20000, 9); target = base[:8000] + rnd(100, 10) + base[8000:]
else:
    raise SystemExit("unknown kind "+kind)
open(bp,"wb").write(base); open(tp,"wb").write(target)
PY
}

PATCHGEN="$ROOT/cli/tinymldelta_patchgen.py"
KINDS="identical scattered growth shrink rle large copyadd_basic copyadd_shift"
if "$PY" -c "import lz4" >/dev/null 2>&1; then KINDS="$KINDS lz4"; fi
fail=0
for kind in $KINDS; do
  gen "$kind" "$WORK/base.bin" "$WORK/target.bin"
  case "$kind" in
    lz4)        EXTRA="--lz4 --lz4-window 4096" ;;
    copyadd_*)  EXTRA="--copy-add" ;;
    *)          EXTRA="" ;;
  esac
  "$PY" "$PATCHGEN" "$WORK/base.bin" "$WORK/target.bin" "$WORK/patch.tmd" --algo crc32 $EXTRA >/dev/null
  ${RUNNER:-} "$WORK/core_apply" "$WORK/base.bin" "$WORK/patch.tmd" "$WORK/out.bin" "$SLOT"
  if cmp -s "$WORK/out.bin" "$WORK/target.bin"; then
    echo "  [PASS] $kind"
  else
    echo "  [FAIL] $kind (reconstructed != target)"; fail=1
  fi
done

# Negative test: a patch built for baseA must be REJECTED when applied onto a
# different baseB (base-digest guard). Expect core_apply to exit non-zero.
"$PY" - "$WORK" <<'PY'
import sys, random
W = sys.argv[1]
def rnd(n, s): r = random.Random(s); return bytes(r.randrange(256) for _ in range(n))
baseA, baseB = rnd(8000, 21), rnd(8000, 22)      # same length, different content
target = bytearray(baseA)
for i in range(100, 300): target[i] ^= 0xAA
open(W + "/baseA.bin", "wb").write(baseA)
open(W + "/baseB.bin", "wb").write(baseB)
open(W + "/targetA.bin", "wb").write(bytes(target))
PY
"$PY" "$PATCHGEN" "$WORK/baseA.bin" "$WORK/targetA.bin" "$WORK/patchA.tmd" --algo crc32 >/dev/null
if ${RUNNER:-} "$WORK/core_apply" "$WORK/baseB.bin" "$WORK/patchA.tmd" "$WORK/out.bin" "$SLOT" >/dev/null 2>&1; then
  echo "  [FAIL] wrongbase (patch for baseA was NOT rejected on baseB)"; fail=1
else
  echo "  [PASS] wrongbase (rejected: base digest mismatch)"
fi

echo
if [ "$fail" -eq 0 ]; then echo "OK — core reconstructs target byte-exact in all cases"; else echo "FAIL"; fi
exit $fail
