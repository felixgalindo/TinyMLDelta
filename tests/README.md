# Tests

Unit + integration tests for TinyMLDelta, run in CI (`.github/workflows/ci.yml`)
and reproducible locally.

## Layout

| Path | What |
|------|------|
| `python/` | pytest for PatchGen: diff/RLE/chunk-splitting units + roundtrip property tests (patch → apply → equals target) |
| `python/applier.py` | pure-Python reference applier used as the roundtrip oracle |
| `c/core_apply_harness.cpp` | applies a `.tmd` via the **real C core** (in-memory flash port) |
| `c/run_core_tests.sh` | drives the core over identical / scattered / growth / shrink / RLE / >64 KB cases, asserting byte-exact reconstruction |
| `run_all.sh` | runs everything CI runs (minus valgrind, which is Linux-only) |

The two `examples/*/` reference implementations also self-test (their `make test`
exits non-zero on any unexpected result) and are run in CI.

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
