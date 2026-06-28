#!/usr/bin/env bash
# Run the full local test suite the way CI does (minus valgrind, which is
# Linux-only). Use this to reproduce CI locally:  tests/run_all.sh
#
#   PY=.tinyenv/bin/python tests/run_all.sh        # pick a python
#   SAN=1 tests/run_all.sh                          # C tests under ASan+UBSan
#
# License: Apache-2.0
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PY="${PY:-python3}"

san_c="" san_cxx=""
if [ "${SAN:-0}" = "1" ]; then
  s="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
  san_c="-std=c99 $s"; san_cxx="-std=c++11 $s"
fi

echo "== Python (PatchGen) tests =="
"$PY" -m pytest "$ROOT/tests/python" -q

echo; echo "== C example self-tests =="
for ex in capability_envelope io_schema; do
  make -C "$ROOT/examples/$ex" clean >/dev/null 2>&1 || true
  if [ -n "$san_c" ]; then make -C "$ROOT/examples/$ex" CFLAGS="$san_c" test;
  else make -C "$ROOT/examples/$ex" test; fi
  make -C "$ROOT/examples/$ex" clean >/dev/null 2>&1 || true
done

echo; echo "== C unit tests =="
ubin="$(mktemp)"
cc ${san_c:--std=c99 -O1} -I "$ROOT/runtime/include" -o "$ubin" "$HERE/c/unit_tests.c"
"$ubin"
rm -f "$ubin"

echo; echo "== C core integration test =="
if [ -n "$san_c" ]; then
  CC="cc" CXX="c++" CFLAGS="$san_c" CXXFLAGS="$san_cxx" PY="$PY" "$HERE/c/run_core_tests.sh"
else
  PY="$PY" "$HERE/c/run_core_tests.sh"
fi

echo; echo "ALL LOCAL TESTS PASSED"
