#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verify that the active model in flash.bin matches target.tflite.

This helper is intentionally layout-agnostic: it does not assume any
specific slot geometry. Instead, it checks whether the target model
bytes appear contiguously anywhere inside the flash image.

Usage:
    python3 verify_flash.py flash.bin path/to/target.tflite

If the target model bytes are found in flash.bin, the verification
passes and prints the offset where it was found.
"""
import sys


def load_bytes(path: str) -> bytes:
    """Load an entire file into memory as bytes."""
    with open(path, "rb") as f:
        return f.read()


def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: verify_flash.py flash.bin target.tflite")
        sys.exit(1)

    flash_path, target_path = sys.argv[1], sys.argv[2]

    flash = load_bytes(flash_path)
    target = load_bytes(target_path)

    idx = flash.find(target)
    if idx == -1:
        print("FAIL: target model bytes not found in flash image.")
        sys.exit(1)

    print(f"PASS: target model found in flash at offset 0x{idx:08X}")
    print(f"flash size = {len(flash)} bytes, target size = {len(target)} bytes")


if __name__ == "__main__":
    main()
