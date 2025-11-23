#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# make_flash.py
#
# Create a simulated NOR flash image for TinyMLDelta POSIX demo.
#
# Author: Felix Galindo 
# -----------------------------------------------------------------------------
"""
Creates a simulated flash image with:
  - total flash size (default: 256 KiB)
  - Slot A containing the base .tflite model
  - Slot B initialized as a copy of Slot A
  - Remaining flash bytes set to 0xFF
"""

import argparse


def main():
    parser = argparse.ArgumentParser(description="Create simulated flash image.")
    parser.add_argument("--flash", required=True, help="Output flash image path")
    parser.add_argument("--base", required=True, help="Base .tflite model path")
    parser.add_argument("--size", type=int, default=262144,
                        help="Total flash size in bytes (default 256 KiB)")
    args = parser.parse_args()

    flash_size = args.size
    slot_size = flash_size // 2  # A/B slots

    # Read base model
    with open(args.base, "rb") as f:
        base_bytes = f.read()

    if len(base_bytes) > slot_size:
        raise RuntimeError(
            f"Base model is too large ({len(base_bytes)} > {slot_size})"
        )

    # Prepare flash contents
    flash = bytearray([0xFF] * flash_size)

    # Slot A: base model at offset 0
    flash[0:len(base_bytes)] = base_bytes

    # Slot B: copy of slot A
    flash[slot_size:slot_size + len(base_bytes)] = base_bytes

    # Write flash image
    with open(args.flash, "wb") as f:
        f.write(flash)

    print(f"[make_flash] Created flash: {args.flash} ({flash_size} bytes)")
    print(f"[make_flash] Slot size: {slot_size} bytes")
    print(f"[make_flash] Base model: {len(base_bytes)} bytes written to A and B")


if __name__ == "__main__":
    main()
