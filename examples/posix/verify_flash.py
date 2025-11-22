#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file verify_flash.py
@brief TinyMLDelta POSIX demo — verify that target model bytes exist in flash.bin.
@author Felix Galindo
@license Apache-2.0

This script is used by the POSIX example to sanity-check that applying a
TinyMLDelta patch produced the expected target model, somewhere inside
the flash image.

By default, it simply checks whether the target model's raw byte sequence
appears as a contiguous subsequence of the flash image.

Optionally, you can pass a patch file path and it will also dump the patch
header for debugging.

Usage:
    python3 verify_flash.py --flash flash.bin --target target.tflite
    python3 verify_flash.py --flash flash.bin --target target.tflite --patch patch.tmd
"""

import argparse
import os
import struct

# Keep this header layout in sync with cli/tinymldelta_patchgen.py and
# runtime/include/tinymldelta_internal.h.
HDR_FMT = "<BBHII32s32sHH"


def read_file(path: str) -> bytes:
    """Read an entire file into memory.

    Args:
        path: Path to the file.

    Returns:
        File contents as bytes.

    Raises:
        OSError: If the file cannot be opened/read.
    """
    with open(path, "rb") as f:
        return f.read()


def debug_print_patch_header(path: str) -> None:
    """Print a parsed TinyMLDelta patch header for debugging.

    Args:
        path: Path to the .tmd patch file.
    """
    try:
        data = read_file(path)
    except OSError as e:
        print(f"[verify_flash] WARNING: could not read patch file {path}: {e}")
        return

    if len(data) < struct.calcsize(HDR_FMT):
        print(f"[verify_flash] WARNING: patch too small for header: {path}")
        return

    hdr_bytes = data[: struct.calcsize(HDR_FMT)]
    v, algo, chunks_n, base_len, target_len, base_chk, tgt_chk, meta_len, flags = \
        struct.unpack(HDR_FMT, hdr_bytes)

    first16 = " ".join(f"{b:02x}" for b in hdr_bytes[:16])

    print("[verify_flash] Patch header:")
    print(f"  file       : {path}")
    print(f"  first16    : {first16}")
    print(f"  v          : {v}")
    print(f"  algo       : {algo}  (0=NONE, 1=CRC32)")
    print(f"  chunks_n   : {chunks_n}")
    print(f"  base_len   : {base_len}")
    print(f"  target_len : {target_len}")
    print(f"  meta_len   : {meta_len}")
    print(f"  flags      : 0x{flags:04x}")


def main() -> int:
    """CLI entry point for verifying flash vs. target model.

    Returns:
        0 on success (target found), non-zero on failure.
    """
    ap = argparse.ArgumentParser(
        description="TinyMLDelta POSIX demo — verify flash.bin contains target model."
    )
    ap.add_argument(
        "--flash",
        required=True,
        help="Path to flash image (e.g. flash.bin)",
    )
    ap.add_argument(
        "--target",
        required=True,
        help="Path to target .tflite model",
    )
    ap.add_argument(
        "--patch",
        required=False,
        help="Optional path to patch file for header debug",
    )

    args = ap.parse_args()

    # Optional patch header dump
    if args.patch:
        debug_print_patch_header(args.patch)

    # Read flash + target
    try:
        flash_bytes = read_file(args.flash)
    except OSError as e:
        print(f"[verify_flash] ERROR: flash image not found: {args.flash} ({e})")
        return 1

    try:
        target_bytes = read_file(args.target)
    except OSError as e:
        print(f"[verify_flash] ERROR: target model not found: {args.target} ({e})")
        return 1

    print(f"[verify_flash] flash:  {args.flash} ({len(flash_bytes)} bytes)")
    print(f"[verify_flash] target: {args.target} ({len(target_bytes)} bytes)")

    # Simple containment check.
    offset = flash_bytes.find(target_bytes)
    if offset < 0:
        print("[verify_flash] ERROR: target model bytes not found in flash image.")
        return 2

    print(f"[verify_flash] SUCCESS: target model found at offset {offset} in flash image.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
