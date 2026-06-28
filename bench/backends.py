#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
backends.py — Compare compression/delta backends on a base->target pair.

For a model update, the deliverable is the *changed* bytes; how you encode them
is the "backend". This measures each backend's patch size AND records whether its
decoder is feasible on an MCU — because a backend is only useful if it ships.

Backends:
  raw        verbatim changed regions (0 decode RAM)
  rle        run-length (current default; ~1 KB streaming decode)
  lz4        LZ4 block compression of the delta payload (small window decoder)
  bsdiff     bsdiff4 whole-file delta (smallest, but O(n) RAM -> NOT on-device)
  gzip_full  gzip of the whole target model (the "just compress it" baseline)

`lz4` / `bsdiff4` are optional; install to enable (otherwise reported as None).

Usage:
  python3 backends.py base.bin target.bin
  # or import measure_backends(base_bytes, target_bytes)

License: Apache-2.0
"""

import gzip
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "cli"))
import tinymldelta_patchgen as pg  # noqa: E402  (find_diffs, rle_encode)


# Static decode-feasibility annotation per backend (the critical second number).
DECODE_RAM = {
    "raw":       "0 B (write-through)",
    "rle":       "~1 KB streaming",
    "lz4":       "~4-16 KB window",
    "bsdiff":    "O(n) — NOT MCU-deployable",
    "gzip_full": "~32 KB window (full model)",
}
MCU_OK = {"raw": True, "rle": True, "lz4": True, "bsdiff": False, "gzip_full": True}


def _delta_payload(base: bytes, target: bytes) -> bytes:
    """Concatenated changed-region bytes — what any delta backend must ship."""
    regions = pg.find_diffs(base, target, merge_gap=16)
    return b"".join(bytes(data) for _off, data in regions)


def measure_backends(base: bytes, target: bytes) -> dict:
    """Return {backend: {bytes, ratio, decode_ram, mcu}} for one pair."""
    payload = _delta_payload(base, target)
    tgt_len = max(len(target), 1)
    out = {}

    def add(name, nbytes):
        out[name] = {
            "bytes": nbytes,
            "ratio": (nbytes / tgt_len) if nbytes is not None else None,
            "decode_ram": DECODE_RAM[name],
            "mcu": MCU_OK[name],
        }

    # raw: payload as-is
    add("raw", len(payload))

    # rle: per-region min(raw, rle), summed (mirrors PatchGen's choice)
    rle_total = 0
    for _off, data in pg.find_diffs(base, target, merge_gap=16):
        enc = pg.rle_encode(bytes(data))
        rle_total += min(len(enc), len(data))
    add("rle", rle_total)

    # lz4 (optional): compress the delta payload
    try:
        import lz4.block as _lz4
        add("lz4", len(_lz4.compress(payload, store_size=False)))
    except Exception:
        add("lz4", None)

    # bsdiff (optional): whole-file delta — size reference, not deployable
    try:
        import bsdiff4
        add("bsdiff", len(bsdiff4.diff(base, target)))
    except Exception:
        add("bsdiff", None)

    # gzip of full target — the "just compress the whole model" baseline
    add("gzip_full", len(gzip.compress(target, 9)))

    return out


def _fmt(v):
    return f"{v:,} B" if isinstance(v, int) else "n/a"


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: backends.py <base.bin> <target.bin>")
    with open(sys.argv[1], "rb") as f:
        base = f.read()
    with open(sys.argv[2], "rb") as f:
        target = f.read()
    res = measure_backends(base, target)
    print(f"target={len(target):,} B   delta-payload={len(_delta_payload(base, target)):,} B\n")
    print(f"  {'backend':10} {'size':>12} {'ratio':>8}  {'decode RAM':24} on-MCU")
    for name in ["raw", "rle", "lz4", "bsdiff", "gzip_full"]:
        r = res[name]
        ratio = f"{r['ratio']*100:.2f}%" if r["ratio"] is not None else "—"
        print(f"  {name:10} {_fmt(r['bytes']):>12} {ratio:>8}  "
              f"{r['decode_ram']:24} {'yes' if r['mcu'] else 'NO'}")


if __name__ == "__main__":
    main()
