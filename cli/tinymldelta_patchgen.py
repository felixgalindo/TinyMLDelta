#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file tinymldelta_patchgen.py
@brief TinyMLDelta — TFLite-first patch generator with optional metadata TLVs.
@author Felix Galindo
@license Apache-2.0

This tool computes a compact binary patch between a "base" and "target"
TFLite model (or any pair of binary blobs) and emits a TinyMLDelta patch
file with the following on-the-wire format:

  [header] [TLVs...] [chunk headers + data...]

Where:

  - The header (tmd_hdr_t) encodes version, digest algorithm, model lengths,
    and digests of base/target.

  - The optional TLV metadata block can include:
        * required tensor arena bytes
        * required TFLM ABI/schema version
        * opset hash
        * I/O hash

  - Each chunk describes a byte run to overwrite at a given offset, with
    optional RLE compression and per-chunk CRC32.

Usage (basic):
    python3 tinymldelta_patchgen.py base.tflite target.tflite patch.tmd

Usage (with auto-metadata from target TFLite):
    python3 tinymldelta_patchgen.py base.tflite target.tflite patch.tmd \\
        --auto-meta --arena-factor 1.4

Manual metadata overrides (take precedence over auto-meta):
    --req-arena BYTES
    --tflm-abi VERSION
    --opset-hash 0x1234abcd
    --io-hash 0xdeadbeef
"""

import argparse
import struct
import zlib
from typing import Optional

# Try to import optional TFLite-aware metadata helper.
# If not available, we degrade gracefully and skip auto-meta.
try:
    from tinymldelta_meta_compute import compute_from_tflite  # type: ignore
except Exception:
    compute_from_tflite = None  # type: ignore

# --------------------------------------------------------------------------- #
#                           Wire-format constants                             #
# --------------------------------------------------------------------------- #

#: Patch header layout (see tmd_hdr_t in C runtime; little-endian).
HDR_FMT = "<BBHII32s32sHH"

#: Chunk header layout (see tmd_chunk_hdr_t in C runtime; little-endian).
CHUNK_FMT = "<IHBb"  # off,len,enc,has_crc

# Digest algorithms (must match runtime enum)
ALGO_NONE = 0
ALGO_CRC32 = 1

# Chunk encoding types (must match runtime enum)
ENC_RAW = 0
ENC_RLE = 1  # [count][byte], count 0 => 256

# Metadata TLV tags (must match tinymldelta_internal.h)
TMD_META_REQ_ARENA_BYTES = 0x01
TMD_META_TFLM_ABI = 0x02
TMD_META_OPSET_HASH = 0x03
TMD_META_IO_HASH = 0x04


# --------------------------------------------------------------------------- #
#                          RLE compression helpers                            #
# --------------------------------------------------------------------------- #

def rle_encode(data: bytes) -> bytes:
    """Encode a byte string using a simple RLE scheme.

    The format is a sequence of (count, value) pairs:
        [count][byte]

    Where:
        - count is in [1..255] encoded as uint8
        - count==0 means 256 (so we can represent runs up to 256 bytes)

    Args:
        data: Input bytes to encode.

    Returns:
        RLE-encoded bytes. May be longer than the input; the caller is
        responsible for choosing between raw vs RLE.
    """
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        val = data[i]
        run = 1
        i += 1
        while i < n and data[i] == val and run < 256:
            run += 1
            i += 1
        out.append(run & 0xFF if run != 256 else 0)
        out.append(val)
    return bytes(out)


# --------------------------------------------------------------------------- #
#                          Diff / coalescing helpers                          #
# --------------------------------------------------------------------------- #

def find_diffs(base: bytes, target: bytes, merge_gap: int = 16):
    """Identify byte ranges where target differs from base.

    The algorithm scans both byte arrays and collects runs where
    base[i:i+len] != target[i:i+len]. Adjacent runs that are close
    (within merge_gap) are merged to reduce chunk count.

    Args:
        base:   Original byte array.
        target: Desired byte array.
        merge_gap: If two diff runs are separated by <= merge_gap bytes,
                   merge them into a single chunk.

    Returns:
        List of (offset, bytes) pairs describing the new data to write.
    """
    diffs = []
    i = 0
    n = min(len(base), len(target))
    while i < n:
        if base[i] != target[i]:
            start = i
            i += 1
            while i < n and base[i] != target[i]:
                i += 1
            diffs.append([start, bytearray(target[start:i])])
        else:
            i += 1

    # If target is longer, append the tail as a diff.
    if len(target) > n:
        diffs.append([n, bytearray(target[n:])])

    if not diffs:
        return []

    # Merge nearby diffs to reduce record count.
    merged = [diffs[0]]
    for off, data in diffs[1:]:
        prev_off, prev_data = merged[-1]
        prev_end = prev_off + len(prev_data)
        if off - prev_end <= merge_gap:
            # Fill any gap then append the new data.
            merged[-1][1].extend(target[prev_end:off])
            merged[-1][1].extend(data)
        else:
            merged.append([off, data])
    return [(off, bytes(data)) for off, data in merged]


# --------------------------------------------------------------------------- #
#                             Metadata (TLV)                                  #
# --------------------------------------------------------------------------- #

def tlv(tag: int, payload: bytes) -> bytes:
    """Build a single TLV record.

    Layout:
        [tag: u8][len: u8][value: len bytes]

    Args:
        tag:     TLV tag (0..255).
        payload: Value bytes; must be <=255 bytes long.

    Returns:
        Encoded TLV record.

    Raises:
        ValueError: If payload is too large (>255 bytes).
    """
    if len(payload) > 255:
        raise ValueError("TLV payload too large (>255 bytes)")
    return struct.pack("<BB", tag & 0xFF, len(payload)) + payload


# --------------------------------------------------------------------------- #
#                         Debug / header inspection                           #
# --------------------------------------------------------------------------- #

def debug_print_patch_header(path: str) -> None:
    """Read and pretty-print the TinyMLDelta patch header.

    This is intended for debugging from the CLI and from the POSIX demo
    (via run_demo.sh / verify_flash.py).

    Args:
        path: Path to the .tmd patch file.
    """
    try:
        with open(path, "rb") as f:
            hdr_bytes = f.read(struct.calcsize(HDR_FMT))
    except OSError as e:
        print(f"[TinyMLDelta] Failed to open patch for header debug: {path} ({e})")
        return

    if len(hdr_bytes) < struct.calcsize(HDR_FMT):
        print(f"[TinyMLDelta] Patch too small to contain header: {path}")
        return

    v, algo, chunks_n, base_len, target_len, base_chk, tgt_chk, meta_len, flags = \
        struct.unpack(HDR_FMT, hdr_bytes)

    # Print a short hex dump of the first 16 bytes
    first16 = " ".join(f"{b:02x}" for b in hdr_bytes[:16])

    print("[TinyMLDelta] Patch header debug:")
    print(f"  file       : {path}")
    print(f"  first16    : {first16}")
    print(f"  v          : {v}")
    print(f"  algo       : {algo}  (0=NONE, 1=CRC32)")
    print(f"  chunks_n   : {chunks_n}")
    print(f"  base_len   : {base_len}")
    print(f"  target_len : {target_len}")
    print(f"  meta_len   : {meta_len}")
    print(f"  flags      : 0x{flags:04x}")


# --------------------------------------------------------------------------- #
#                              Main entry point                               #
# --------------------------------------------------------------------------- #

def main() -> None:
    """CLI entry point for TinyMLDelta patch generator.

    It:
      1) Reads the base and target model bytes.
      2) Computes byte-level diffs (and merges nearby runs).
      3) Optionally derives TFLite metadata (arena, ABI, opset, I/O hash).
      4) Applies manual metadata overrides, if provided.
      5) Encodes everything into the TinyMLDelta patch wire format.
    """
    ap = argparse.ArgumentParser(
        description="TinyMLDelta patch generator (TFLite-first)."
    )
    ap.add_argument("base", help="base .tflite path (or any binary)")
    ap.add_argument("target", help="target .tflite path (or any binary)")
    ap.add_argument("out", help="output patch file path")
    ap.add_argument(
        "--algo",
        choices=["none", "crc32"],
        default="crc32",
        help="header digest mode (default: crc32)",
    )
    ap.add_argument(
        "--merge-gap",
        type=int,
        default=16,
        help="merge diffs closer than this many bytes",
    )
    ap.add_argument(
        "--min-chunk",
        type=int,
        default=8,
        help="coalesce tiny diffs into their predecessor if nearby",
    )

    # Auto metadata from the target TFLite model
    ap.add_argument(
        "--auto-meta",
        action="store_true",
        help="derive TLVs from target .tflite using tinymldelta_meta_compute.py",
    )
    ap.add_argument(
        "--arena-factor",
        type=float,
        default=1.4,
        help="heuristic multiplier for arena estimate; 0=disable",
    )

    # Manual overrides (take precedence over auto-meta)
    ap.add_argument(
        "--req-arena",
        type=int,
        default=None,
        help="required tensor arena bytes for target model (u32)",
    )
    ap.add_argument(
        "--tflm-abi",
        type=int,
        default=None,
        help="required TFLM ABI/schema version (u16)",
    )
    ap.add_argument(
        "--opset-hash",
        type=lambda x: int(x, 0),
        default=None,
        help="required opset hash (u32, e.g. 0x1234abcd)",
    )
    ap.add_argument(
        "--io-hash",
        type=lambda x: int(x, 0),
        default=None,
        help="I/O shape/type hash (u32)",
    )

    args = ap.parse_args()

    # 1) Load models
    with open(args.base, "rb") as f:
        base = f.read()
    with open(args.target, "rb") as f:
        target = f.read()

    # 2) Compute raw diffs
    diffs = find_diffs(base, target, merge_gap=args.merge_gap)

    # 3) Coalesce very small diffs into their predecessor if close enough
    coalesced = []
    for off, data in diffs:
        if (
            coalesced
            and (len(data) < args.min_chunk)
            and (off <= coalesced[-1][0] + len(coalesced[-1][1]) + args.merge_gap)
        ):
            prev_off, prev_data = coalesced[-1]
            gap = target[prev_off + len(prev_data):off]
            coalesced[-1] = (prev_off, prev_data + gap + data)
        else:
            coalesced.append((off, data))
    diffs = coalesced

    # 4) Header digests
    if args.algo == "crc32":
        base_chk = struct.pack("<I", zlib.crc32(base) & 0xFFFFFFFF) + b"\x00" * 28
        tgt_chk = struct.pack("<I", zlib.crc32(target) & 0xFFFFFFFF) + b"\x00" * 28
        algo = ALGO_CRC32
        chunk_has_crc = 1
    else:
        base_chk = b"\x00" * 32
        tgt_chk = b"\x00" * 32
        algo = ALGO_NONE
        chunk_has_crc = 0

    # 5) Auto metadata (TFLite-aware), if requested
    auto_req_arena = auto_abi = auto_opset = auto_io = None
    if args.auto_meta:
        if compute_from_tflite is None:
            print("[TinyMLDelta] Warning: auto-meta requested but "
                  "tinymldelta_meta_compute.compute_from_tflite() is unavailable.")
        else:
            try:
                auto_abi, auto_opset, auto_io, auto_req_arena = compute_from_tflite(
                    args.target,
                    None
                    if args.arena_factor is None or args.arena_factor <= 0
                    else float(args.arena_factor),
                )
            except Exception as e:
                print(f"[TinyMLDelta] Auto-metadata unavailable: {e}")
                auto_req_arena = auto_abi = auto_opset = auto_io = None

    # 6) Manual overrides take precedence over auto-meta
    req_arena = args.req_arena if args.req_arena is not None else (auto_req_arena or 0)
    tflm_abi = args.tflm_abi if args.tflm_abi is not None else (auto_abi or 0)
    opset = args.opset_hash if args.opset_hash is not None else (auto_opset or 0)
    io_hash = args.io_hash if args.io_hash is not None else (auto_io or 0)

    # 7) Build metadata TLVs
    meta = bytearray()
    if req_arena and req_arena > 0:
        meta += tlv(TMD_META_REQ_ARENA_BYTES, struct.pack("<I", int(req_arena)))
    if tflm_abi and tflm_abi > 0:
        meta += tlv(TMD_META_TFLM_ABI, struct.pack("<H", int(tflm_abi & 0xFFFF)))
    if opset and opset > 0:
        meta += tlv(TMD_META_OPSET_HASH, struct.pack("<I", int(opset & 0xFFFFFFFF)))
    if io_hash and io_hash > 0:
        meta += tlv(TMD_META_IO_HASH, struct.pack("<I", int(io_hash & 0xFFFFFFFF)))

    v = 1
    meta_len = len(meta)
    flags = 0

    # 8) Encode chunks with optional RLE
    chunks = []
    for off, raw in diffs:
        rle = rle_encode(raw)
        if len(rle) < len(raw):
            enc = ENC_RLE
            data = rle
        else:
            enc = ENC_RAW
            data = raw
        chunks.append((off, enc, data))

    # 9) Write final patch
    with open(args.out, "wb") as out:
        hdr = struct.pack(
            HDR_FMT,
            v,
            algo,
            len(chunks),
            len(base),
            len(target),
            base_chk,
            tgt_chk,
            meta_len,
            flags,
        )
        out.write(hdr)
        if meta_len:
            out.write(meta)
        for off, enc, data in chunks:
            chdr = struct.pack(CHUNK_FMT, off, len(data), enc, chunk_has_crc)
            out.write(chdr)
            if chunk_has_crc:
                out.write(struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF))
            out.write(data)

    print(f"TinyMLDelta patch written: {args.out}")
    print(
        f"Chunks: {len(chunks)}  "
        f"Encoded bytes: {sum(len(d) for _, _, d in chunks)}  "
        f"Meta: {meta_len} bytes"
    )

    # 10) Debug header dump (so you can see what runtime will parse)
    debug_print_patch_header(args.out)

    if args.auto_meta:
        print(
            f"Auto-meta (resolved): req_arena={req_arena} bytes, "
            f"tflm_abi={tflm_abi}, opset_hash=0x{opset:08x}, io_hash=0x{io_hash:08x}"
        )


if __name__ == "__main__":
    main()
