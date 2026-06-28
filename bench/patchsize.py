#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patchsize.py — Measure TinyMLDelta patch size vs. baselines for a model pair.

For a (base, target) TFLite pair this reports:
  - TinyMLDelta patch size (the real .tmd produced by the project's PatchGen)
  - gzip of the full target model (the "just compress the whole thing" baseline)
  - optional bsdiff4 / detools patches if those packages are installed

All sizes are in bytes; ratio is patch/target.

Author:  Felix Galindo
License: Apache-2.0
"""

import gzip
import os
import struct
import subprocess
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_PATCHGEN = os.path.join(_HERE, "..", "cli", "tinymldelta_patchgen.py")


def _run_patchgen(base_path: str, target_path: str, out_path: str) -> int:
    """Run the project's PatchGen; return chunk count read from the .tmd header.

    chunks_n is the uint16 at byte offset 2 of tmd_hdr_t ("<BBHII32s32sHH").
    Reading it from the file is more robust than parsing stdout (PatchGen can
    exit nonzero on a benign post-write step).
    """
    cmd = [sys.executable, _PATCHGEN, base_path, target_path, out_path, "--algo", "crc32"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if not os.path.exists(out_path):
        sys.stderr.write(res.stderr)
        return -1
    with open(out_path, "rb") as f:
        hdr = f.read(4)
    return struct.unpack("<H", hdr[2:4])[0] if len(hdr) >= 4 else -1


def measure(base: bytes, target: bytes) -> dict:
    """Return a dict of patch sizes for one base->target pair."""
    with tempfile.TemporaryDirectory() as td:
        bp = os.path.join(td, "base.tflite")
        tp = os.path.join(td, "target.tflite")
        pp = os.path.join(td, "patch.tmd")
        with open(bp, "wb") as f:
            f.write(base)
        with open(tp, "wb") as f:
            f.write(target)

        chunks = _run_patchgen(bp, tp, pp)
        tmd_size = os.path.getsize(pp) if os.path.exists(pp) else -1

        row = {
            "base_bytes": len(base),
            "target_bytes": len(target),
            "tmd_bytes": tmd_size,
            "tmd_chunks": chunks,
            "tmd_ratio": (tmd_size / len(target)) if tmd_size > 0 else None,
            "gzip_full_bytes": len(gzip.compress(target, 9)),
        }

        # Optional: bsdiff4 (gold-standard small patch, but O(n) RAM on device)
        try:
            import bsdiff4
            row["bsdiff_bytes"] = len(bsdiff4.diff(base, target))
        except Exception:
            row["bsdiff_bytes"] = None

        # Optional: detools (streaming delta)
        try:
            import io
            import detools
            buf = io.BytesIO()
            detools.create_patch(io.BytesIO(base), io.BytesIO(target), buf)
            row["detools_bytes"] = len(buf.getvalue())
        except Exception:
            row["detools_bytes"] = None

        return row
