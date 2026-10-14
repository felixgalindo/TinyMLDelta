#!/usr/bin/env python3
"""
Host-side driver for the TinyMLDelta SensorTile (STM32L4) example.

Everything here goes over SWD with the stlink tools, and every SWD operation
uses --connect-under-reset because the SensorTile's firmware sleeps in STOP 2
with the debug port unclocked.

Subcommands
-----------
  provision MODEL.tflite   erase the TinyMLDelta regions and load MODEL into
                           both slots, leaving slot A active
  stage PATCH.tmd          wrap a .tmd in the staging header and write it to
                           the staging region
  readslot A|B OUT.bin     read a slot back out of the device
  verify EXPECT.tflite     read the ACTIVE slot and compare it byte-for-byte
                           against EXPECT.tflite
  status                   halt the target and dump its status block + log
  run PATCH.tmd            stage, reset, wait for completion, then show status
                           and verify — the whole cycle in one command
  reset                    just restart the target

Timing note
-----------
The firmware runs from the MSI oscillator at 4 MHz (no PLL, nothing to
misconfigure), and it CRC32s two 32 KiB slots twice per boot in software. A
full patch cycle therefore takes ~15-25 s. `status` halts the CPU to read its
RAM, so calling it too early captures a half-finished run — which looks exactly
like a corrupted result. That is what --wait is for.

Nothing here ever touches option bytes, so the board stays recoverable.

Author: Harshdeep Parmar
License: Apache-2.0
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

# --- must match inc/flash_layout.h -----------------------------------------
SLOT_A_ADDR   = 0x08060000
SLOT_B_ADDR   = 0x08068000
SLOT_BYTES    = 32 * 1024
JOURNAL_ADDR  = 0x08070000
MARKER_ADDR   = 0x08070800
STAGING_ADDR  = 0x08078000
STAGING_BYTES = 32 * 1024
PAGE          = 2048

# --- must match src/main.c --------------------------------------------------
STAGE_MAGIC   = 0x53444D54          # 'TMDS'
STAGE_HDR_SZ  = 16

# --- must match src/tmd_port_stm32l4.c --------------------------------------
MARKER_MAGIC  = 0x41444D54          # 'TMDA'

# --- must match board.h / the .elf ------------------------------------------
STATUS_MAGIC  = 0x31534D54          # 'TMS1'

CUR = pathlib.Path(__file__).resolve().parent
ELF = CUR.parent / "build" / "tmd-stm32l4.elf"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def st(*args, timeout=180):
    cmd = ["st-flash", "--connect-under-reset", *args]
    r = run(cmd, timeout=timeout)
    if r.returncode != 0:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stderr}")
    return r


def write_blob(addr, data, label):
    """Write bytes at a flash address. st-flash erases the pages it covers."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(data)
        tmp = f.name
    try:
        st("write", tmp, hex(addr))
        print(f"  wrote {len(data):,} bytes to 0x{addr:08X}  ({label})")
    finally:
        pathlib.Path(tmp).unlink(missing_ok=True)


def read_blob(addr, length):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        tmp = f.name
    try:
        st("read", tmp, hex(addr), hex(length))
        return pathlib.Path(tmp).read_bytes()
    finally:
        pathlib.Path(tmp).unlink(missing_ok=True)


def elf_sym(name):
    """Address of a symbol, straight out of the built .elf."""
    if not ELF.exists():
        sys.exit(f"{ELF} not found — run `make` first")
    r = run(["arm-none-eabi-nm", str(ELF)])
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"symbol {name!r} not found in {ELF}")


def openocd(commands, timeout=120, under_reset=False):
    """Run OpenOCD commands against the target.

    under_reset=False is the default and it matters: `connect_assert_srst`
    asserts NRST during init, so *every* such invocation restarts the firmware.
    That is essential for the factory ALLMEMS1 image (which sleeps in STOP 2
    with the debug port unclocked) but actively wrong here — polling the device
    with it kept resetting the board mid-patch and re-running the firmware from
    scratch. This firmware never sleeps, so a plain attach works and leaves it
    running.
    """
    cmd = [
        "openocd",
        "-f", "interface/stlink.cfg",
        "-c", "transport select hla_swd",
        "-c", "adapter speed 480",
        "-f", "target/stm32l4x.cfg",
    ]
    if under_reset:
        cmd += ["-c", "reset_config srst_only srst_nogate connect_assert_srst"]
    cmd += ["-c", "init"]
    for c in commands:
        cmd += ["-c", c]
    cmd += ["-c", "exit"]
    return run(cmd, timeout=timeout)


# ---------------------------------------------------------------- commands

def cmd_provision(args):
    model = pathlib.Path(args.model).read_bytes()
    if len(model) > SLOT_BYTES:
        sys.exit(f"model is {len(model)} bytes, slot is only {SLOT_BYTES}")

    print(f"model: {args.model}  {len(model):,} bytes  crc32=0x{zlib.crc32(model):08x}")

    # Pad each slot image to a whole number of pages so the erase that st-flash
    # performs covers exactly the slot and nothing beyond it.
    padded = model + b"\xFF" * (SLOT_BYTES - len(model))

    print("provisioning:")
    write_blob(SLOT_A_ADDR, padded, "slot A")
    write_blob(SLOT_B_ADDR, padded, "slot B")

    # Erased journal = no update in flight. Erased marker = slot A active.
    write_blob(JOURNAL_ADDR, b"\xFF" * PAGE, "journal (cleared)")
    write_blob(MARKER_ADDR,  b"\xFF" * PAGE, "marker  (-> slot A)")
    write_blob(STAGING_ADDR, b"\xFF" * PAGE, "staging (cleared)")

    print("done — slot A active, both slots hold the base model")


def cmd_stage(args):
    patch = pathlib.Path(args.patch).read_bytes()
    if len(patch) < 80:
        sys.exit("not a TinyMLDelta patch (shorter than the 80-byte header)")
    if len(patch) > STAGING_BYTES - STAGE_HDR_SZ:
        sys.exit("patch too large for the staging region")

    crc = zlib.crc32(patch)
    hdr = struct.pack("<IIII", STAGE_MAGIC, len(patch), crc, 0)

    v, algo, chunks, base_len, target_len = struct.unpack("<BBHII", patch[:12])
    print(f"patch: {args.patch}  {len(patch):,} bytes  crc32=0x{crc:08x}")
    print(f"  v={v} algo={algo} chunks={chunks} base_len={base_len} target_len={target_len}")

    blob = hdr + patch
    blob += b"\xFF" * ((-len(blob)) % 8)          # doubleword-align the tail
    write_blob(STAGING_ADDR, blob, "staging")
    print("staged — reset the board to apply")


def cmd_readslot(args):
    addr = SLOT_A_ADDR if args.slot.upper() == "A" else SLOT_B_ADDR
    data = read_blob(addr, SLOT_BYTES)
    pathlib.Path(args.out).write_bytes(data)
    print(f"slot {args.slot.upper()} -> {args.out}  crc32(full slot)=0x{zlib.crc32(data):08x}")


def active_slot():
    m = read_blob(MARKER_ADDR, 16)
    magic, slot = struct.unpack("<II", m[:8])
    return 0 if magic != MARKER_MAGIC else (slot & 1)


def cmd_verify(args):
    expect = pathlib.Path(args.expect).read_bytes()
    idx = active_slot()
    addr = SLOT_A_ADDR if idx == 0 else SLOT_B_ADDR

    print(f"active slot: {'AB'[idx]}  (0x{addr:08X})")
    data = read_blob(addr, SLOT_BYTES)
    got = data[:len(expect)]

    print(f"expected : {args.expect}  {len(expect):,} bytes  crc32=0x{zlib.crc32(expect):08x}")
    print(f"on device: {len(got):,} bytes  crc32=0x{zlib.crc32(got):08x}")

    if got == expect:
        print("\n  RESULT: BYTE-EXACT MATCH — the device reproduced the target model\n")
        return 0

    diff = [i for i, (a, b) in enumerate(zip(got, expect)) if a != b]
    print(f"\n  RESULT: MISMATCH — {len(diff):,} differing bytes")
    if diff:
        print(f"  first difference at offset {diff[0]}")
    return 1


def cmd_status(args):
    g_status = elf_sym("g_status")
    s_log    = elf_sym("s_log")

    with tempfile.TemporaryDirectory() as d:
        sp = f"{d}/status.bin"
        lp = f"{d}/log.bin"
        # Plain attach + halt: do NOT reset, or we destroy the very run we are
        # trying to inspect. The firmware sits in its blink loop when finished,
        # so halting it there is harmless and we resume afterwards.
        r = openocd([
            "halt",
            f"dump_image {sp} 0x{g_status:08x} 40",
            f"dump_image {lp} 0x{s_log:08x} 16384",
            "resume",
        ])
        if not pathlib.Path(sp).exists():
            sys.exit(f"could not read target RAM:\n{r.stderr}")

        raw = pathlib.Path(sp).read_bytes()
        log = pathlib.Path(lp).read_bytes()

    (magic, boots, before, after, crc_a, crc_b,
     plen, pcrc, status, loglen) = struct.unpack("<IIIIIIIIiI", raw[:40])

    if magic != STATUS_MAGIC:
        sys.exit(f"status block magic is 0x{magic:08x}, expected 0x{STATUS_MAGIC:08x} "
                 "— firmware may not have run yet")

    names = {0: "OK", 1: "ERR_PARAM", 2: "ERR_HDR", 3: "ERR_INTEGRITY",
             4: "ERR_GUARDRAIL", 5: "ERR_FLASH", 6: "ERR_UNSUPPORTED",
             7: "ERR_SIGNATURE", 8: "ERR_INTERNAL", -1: "not attempted"}

    print("=== device status block ===")
    print(f"  boot count       : {boots}")
    print(f"  active before    : {'AB'[before & 1]}")
    print(f"  active after     : {'AB'[after & 1]}")
    print(f"  crc32 slot A     : 0x{crc_a:08x}")
    print(f"  crc32 slot B     : 0x{crc_b:08x}")
    print(f"  patch length     : {plen:,}")
    print(f"  patch crc32      : 0x{pcrc:08x}")
    print(f"  apply status     : {names.get(status, status)} ({status})")

    text = log[:loglen].decode("utf-8", errors="replace") if loglen else ""
    print(f"\n=== device log ({loglen} bytes) ===")
    print(text)

    # The openocd "resume" above already left the board running.


def cmd_run(args):
    """stage -> reset -> wait -> status -> verify, in one go."""
    import time

    cmd_stage(args)

    print("\nresetting target to apply...")
    run(["st-flash", "reset"], timeout=60)

    # The firmware signals completion by ending its log with "=== done ===".
    # Poll for that rather than guessing a sleep.
    deadline = time.time() + args.wait
    while time.time() < deadline:
        time.sleep(3)
        if _log_complete():
            break
    else:
        print(f"warning: no completion marker after {args.wait}s — showing what there is")

    print()
    cmd_status(args)

    if args.expect:
        print()
        ns = argparse.Namespace(expect=args.expect)
        return cmd_verify(ns)
    return 0


def _log_complete():
    """True once the device log ends with the firmware's completion marker.

    No halt: SWD reads RAM while the core keeps running, so polling never
    stops the CPU in the middle of a flash erase or program."""
    s_log = elf_sym("s_log")
    with tempfile.TemporaryDirectory() as d:
        lp = f"{d}/log.bin"
        openocd([f"dump_image {lp} 0x{s_log:08x} 16384"])
        f = pathlib.Path(lp)
        if not f.exists():
            return False
        txt = f.read_bytes().split(b"\x00")[0]
    return b"=== done ===" in txt


def cmd_reset(args):
    run(["st-flash", "reset"], timeout=60)
    print("target reset")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("provision"); p.add_argument("model");  p.set_defaults(fn=cmd_provision)
    p = sub.add_parser("stage");     p.add_argument("patch");  p.set_defaults(fn=cmd_stage)
    p = sub.add_parser("readslot");  p.add_argument("slot", choices=list("ABab")); p.add_argument("out"); p.set_defaults(fn=cmd_readslot)
    p = sub.add_parser("verify");    p.add_argument("expect"); p.set_defaults(fn=cmd_verify)
    p = sub.add_parser("status");    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("run")
    p.add_argument("patch")
    p.add_argument("--expect", help="target .tflite to verify against afterwards")
    p.add_argument("--wait", type=int, default=60, help="seconds to wait for completion")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("reset");     p.set_defaults(fn=cmd_reset)

    args = ap.parse_args()
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()
