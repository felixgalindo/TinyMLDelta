# Tests

Unit + integration tests for TinyMLDelta, run in CI (`.github/workflows/ci.yml`)
and reproducible locally.

## Layout

| Path | What |
|------|------|
| `python/` | pytest for PatchGen: diff/RLE/chunk-splitting units, atomic-write, **LZ4** + **COPY/ADD** + **ONNX** roundtrips (patch → apply → equals target) |
| `python/applier.py` | pure-Python reference applier (oracle): positional + RLE + LZ4 + COPY/ADD |
| `c/unit_tests.c` | **per-function C unit tests** (statics via `#include` of the core): `tmd_rle_decoded_len`, `tmd_rle_decode_write`, `tmd_lz4_block_decode`, `tmd_check_guardrails`, `tmd_verify_sig` |
| `c/core_apply_harness.cpp` | applies a `.tmd` via the **real C core** (in-memory flash port) |
| `c/run_core_tests.sh` | drives the core byte-exact over identical / scattered / growth / shrink / RLE / >64 KB / **copyadd** / **lz4** cases, plus a **wrongbase** rejection (base-digest) negative test |
| `run_all.sh` | runs everything CI runs (minus valgrind, which is Linux-only) |

The two `examples/*/` reference implementations also self-test (their `make test`
exits non-zero on any unexpected result) and are run in CI.

Coverage: 24 pytest + 17 C unit + 10 C integration cases + 2 example self-tests.

## Run locally

```bash
# everything (uses your python; pass PY to choose one)
PY=.tinyenv/bin/python tests/run_all.sh

# under AddressSanitizer + UndefinedBehaviorSanitizer
SAN=1 PY=.tinyenv/bin/python tests/run_all.sh

# just the Python suite with coverage
.tinyenv/bin/python -m pytest tests/python -q --cov --cov-config=tests/.coveragerc
```

First time: `pip install -r tests/requirements.txt` (pytest, pytest-cov).

## What CI runs

| Job | Tools |
|-----|-------|
| `python-tests` | pytest + coverage (xml artifact) |
| `c-sanitizers` (gcc, clang) | examples + core under ASan + UBSan |
| `c-valgrind` | examples + core under valgrind (`--leak-check=full`) |
| `c-coverage` | core built `--coverage`, lcov HTML artifact |

## Notes

- valgrind and lcov are Linux-only; on macOS use `SAN=1` for memory checking.
- The C core test is TF-free: base/target are arbitrary byte blobs, so it
  exercises the wire format and apply path without TensorFlow.
