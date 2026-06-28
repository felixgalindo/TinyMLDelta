"""Unit tests for PatchGen internals (diff, RLE, chunk splitting)."""
import importlib

import pytest

pg = importlib.import_module("tinymldelta_patchgen")


# --- RLE -------------------------------------------------------------------- #

def _rle_decode(payload: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(payload):
        count = payload[i] or 256
        out += bytes([payload[i + 1]]) * count
        i += 2
    return bytes(out)


@pytest.mark.parametrize("data", [
    b"\x00" * 100,
    b"\xAB" * 256,            # run of exactly 256 (count encoded as 0)
    b"\xAB" * 300,
    bytes(range(50)),         # no runs — RLE should be larger
    b"aaabbbcccd",
])
def test_rle_roundtrip(data):
    assert _rle_decode(pg.rle_encode(data)) == data


def test_rle_run_of_256():
    enc = pg.rle_encode(b"\x07" * 256)
    assert enc[0] == 0  # count 0 means 256
    assert _rle_decode(enc) == b"\x07" * 256


# --- find_diffs ------------------------------------------------------------- #

def test_find_diffs_identical():
    assert pg.find_diffs(b"abcdef", b"abcdef") == []


def test_find_diffs_single_region():
    base = b"AAAAAAAAAA"
    target = b"AAAXXXXAAA"
    diffs = pg.find_diffs(base, target, merge_gap=0)
    assert len(diffs) == 1
    off, data = diffs[0]
    assert off == 3 and data == b"XXXX"


def test_find_diffs_merge_gap():
    base = b"A" * 20
    target = bytearray(base)
    target[2] = ord("X")
    target[6] = ord("Y")           # 3 bytes apart
    one = pg.find_diffs(base, bytes(target), merge_gap=8)
    many = pg.find_diffs(base, bytes(target), merge_gap=0)
    assert len(one) == 1           # merged
    assert len(many) == 2          # not merged


def test_find_diffs_growth_tail():
    base = b"A" * 10
    target = b"A" * 10 + b"BBBB"
    diffs = pg.find_diffs(base, target)
    assert diffs[-1][0] == 10 and diffs[-1][1] == b"BBBB"


# --- uint16 chunk-length regression ---------------------------------------- #

def test_large_region_does_not_exceed_uint16(make_patch):
    """A >64 KB contiguous diff must be split, not crash (was a struct.error)."""
    base = bytes(200_000)
    target = bytearray(base)
    for i in range(5_000, 180_000):
        target[i] = 0xAB
    patch = make_patch(base, bytes(target))   # would raise if patchgen crashed
    assert len(patch) > 0
