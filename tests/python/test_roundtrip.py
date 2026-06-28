"""Roundtrip property tests: apply(base, patch(base, target)) == target.

Covers weight-style updates (same length, scattered changes), growth, shrink,
highly-compressible (RLE) diffs, and the >64 KB region case.
"""
import random

import pytest

from applier import apply_patch


def _rand(n, seed):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(n))


def test_roundtrip_identical(make_patch):
    base = _rand(2000, 1)
    assert apply_patch(base, make_patch(base, base)) == base


def test_roundtrip_scattered_changes(make_patch):
    base = bytearray(_rand(8000, 2))
    target = bytearray(base)
    rng = random.Random(3)
    for _ in range(200):                      # ~weight-update pattern
        target[rng.randrange(len(target))] ^= 0xFF
    target = bytes(target)
    assert apply_patch(bytes(base), make_patch(bytes(base), target)) == target


def test_roundtrip_growth(make_patch):
    base = _rand(4000, 4)
    target = base + _rand(1500, 5)            # model grew
    assert apply_patch(base, make_patch(base, target)) == target


def test_roundtrip_shrink(make_patch):
    base = _rand(4000, 6)
    target = base[:2500]                      # model shrank
    assert apply_patch(base, make_patch(base, target)) == target


def test_roundtrip_rle_region(make_patch):
    base = _rand(6000, 7)
    target = bytearray(base)
    target[1000:3000] = b"\x00" * 2000        # large compressible run
    target = bytes(target)
    assert apply_patch(base, make_patch(base, target)) == target


def test_roundtrip_large_region(make_patch):
    base = bytes(200_000)
    target = bytearray(base)
    for i in range(5_000, 180_000):           # >64 KB contiguous (split path)
        target[i] = 0xAB
    target = bytes(target)
    assert apply_patch(base, make_patch(base, target)) == target


def test_roundtrip_lz4(make_patch):
    """LZ4-encoded chunks reconstruct the target byte-exact."""
    pytest.importorskip("lz4")            # PatchGen needs lz4 to encode
    base = _rand(6000, 9)
    target = bytearray(base)
    target[1000:4500] = b"\x00" * 3500    # large compressible run (LZ4 matches)
    target[5000:5800] = (b"ABCD" * 200)[:800]
    target = bytes(target)
    patch = make_patch(base, target, extra=["--lz4", "--lz4-window", "4096"])
    assert apply_patch(base, patch) == target


def test_roundtrip_copyadd_basic(make_patch):
    """COPY/ADD reconstructs a same-length weight-style update byte-exact."""
    base = _rand(8000, 13)
    target = bytearray(base)
    rng = random.Random(14)
    for _ in range(60):
        target[rng.randrange(len(target))] ^= 0xFF
    target = bytes(target)
    patch = make_patch(base, target, extra=["--copy-add"])
    assert apply_patch(base, patch) == target


def test_roundtrip_copyadd_offset_shift(make_patch):
    """A mid-stream INSERT cascades offsets: positional byte-diff explodes, but
    COPY/ADD stays tiny (and still reconstructs byte-exact)."""
    base = _rand(20000, 11)
    target = base[:8000] + _rand(100, 12) + base[8000:]   # insert -> shift
    ca = make_patch(base, target, extra=["--copy-add"])
    assert apply_patch(base, ca) == target
    positional = make_patch(base, target)
    assert len(ca) < len(positional) / 5                  # structure-aware wins big


def test_roundtrip_many_sizes(make_patch):
    for seed in range(8):
        n = 500 + seed * 1500
        base = _rand(n, 100 + seed)
        target = bytearray(base)
        rng = random.Random(200 + seed)
        for _ in range(n // 50):
            target[rng.randrange(n)] = rng.randrange(256)
        target = bytes(target)
        assert apply_patch(base, make_patch(base, target)) == target
