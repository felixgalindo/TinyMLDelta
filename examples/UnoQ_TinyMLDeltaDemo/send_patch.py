#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
send_patch.py — Send a TinyMLDelta patch to an Arduino over Serial.

Protocol
--------
  1. Opens the serial port.
  2. Sends the 'u' command to put the device into Update mode.
  3. Sends a 4-byte little-endian length prefix.
  4. Sends the patch bytes.
  5. Reads and prints device responses until the update completes or times out.

The device-side handler (update_start in the sketch) expects exactly this
sequence and will print "[UPDATE] New model active." on success.

Usage
-----
  python3 send_patch.py <port> <patch.tmd>

Examples:
  python3 send_patch.py /dev/ttyACM0 patch.tmd
  python3 send_patch.py COM4 patch.tmd
  python3 send_patch.py /dev/ttyACM0 patch.tmd --baud 115200

Requirements:
  pip install pyserial

Author:  Felix Galindo
License: Apache-2.0
"""

import argparse
import struct
import sys
import time

# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Send a TinyMLDelta patch to Arduino over Serial."
    )
    ap.add_argument("port",  help="Serial port (e.g. /dev/ttyACM0 or COM4)")
    ap.add_argument("patch", help="Path to .tmd patch file")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    ap.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="Seconds to wait for device response after sending (default: 20)",
    )
    ap.add_argument(
        "--no-trigger",
        action="store_true",
        help="Skip sending 'u' — device is already in Update mode",
    )
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("pyserial not found. Run:  pip install pyserial")

    # Load patch
    try:
        with open(args.patch, "rb") as f:
            patch_data = f.read()
    except OSError as e:
        sys.exit(f"Cannot read patch file: {e}")

    print(f"Patch file : {args.patch}")
    print(f"Patch size : {len(patch_data)} bytes")
    print(f"Port       : {args.port}  @{args.baud} baud")
    print()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"Cannot open port: {e}")

    # Brief pause for Arduino auto-reset on open (UNO R4 USB CDC)
    time.sleep(2.0)

    # Drain any pending output
    ser.reset_input_buffer()

    if not args.no_trigger:
        print("Sending 'u' to enter Update mode...")
        ser.write(b"u")
        ser.flush()
        time.sleep(0.3)

    # Send 4-byte little-endian length prefix
    length_prefix = struct.pack("<I", len(patch_data))
    print("Sending length prefix...")
    ser.write(length_prefix)
    ser.flush()

    # Send patch bytes
    print("Sending patch bytes...")
    ser.write(patch_data)
    ser.flush()
    print(f"  Sent {len(patch_data)} bytes.")

    # Read responses until timeout or completion keyword
    print()
    print("Device output:")
    print("-" * 40)

    deadline = time.time() + args.timeout
    success  = False

    while time.time() < deadline:
        line = ser.readline()
        if line:
            text = line.decode(errors="replace").rstrip()
            print(f"  {text}")
            if "New model active" in text or "updated" in text.lower():
                success = True
                break
            if "FAILED" in text or "Aborted" in text:
                break
            # Reset deadline on any activity
            deadline = time.time() + args.timeout

    print("-" * 40)
    ser.close()

    if success:
        print("\nModel update successful.")
        print("Send 'i' in the Serial monitor to start inference with the new model.")
    else:
        print("\nUpdate did not confirm success within timeout.")
        print("Check Serial monitor for more detail.")
        sys.exit(1)


if __name__ == "__main__":
    main()
