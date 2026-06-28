"""Roundtrip property tests: apply(base, patch(base, target)) == target.

Covers weight-style updates (same length, scattered changes), growth, shrink,
highly-compressible (RLE) diffs, and the >64 KB region case.
"""
import random

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
