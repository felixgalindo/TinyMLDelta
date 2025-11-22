#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TinyMLDelta — Weight-Aware Patch Generator (TFLite first).

Generates binary patches **only for model weights**, ignoring:
  - FlatBuffer metadata drift
  - padding/alignment changes
  - converter-version differences
  - reordered operator tables
  - added/removed optional metadata blocks

Output:
  [header] [TLVs...] [chunk headers + data...]
"""
import argparse
import struct
import zlib
from typing import Optional, List, Tuple

import tflite  # FlatBuffer schema
from tinymldelta_meta_compute import compute_from_tflite

HDR_FMT   = "<BBHII32s32sHH"
CHUNK_FMT = "<IHBb"  # off,len,enc,has_crc

ALGO_NONE  = 0
ALGO_CRC32 = 1

ENC_RAW = 0
ENC_RLE = 1  # [count][byte], count 0 => 256

TMD_META_REQ_ARENA_BYTES = 0x01
TMD_META_TFLM_ABI        = 0x02
TMD_META_OPSET_HASH      = 0x03
TMD_META_IO_HASH         = 0x04


# ---------------------------------------------------------------------------
# 🔍 WEIGHT-AWARE LOGIC
# ---------------------------------------------------------------------------

def tflite_weight_ranges(model_buf: bytes) -> List[Tuple[int, int]]:
    """Extract byte ranges corresponding to constant tensors (weights)."""
    model = tflite.Model.Model.GetRootAsModel(model_buf, 0)
    sg = model.Subgraphs(0)

    buffers = model.BuffersLength()
    ranges = []

    # Iterate all tensors
    for t_idx in range(sg.TensorsLength()):
        tensor = sg.Tensors(t_idx)
        buf_idx = tensor.Buffer()
        if buf_idx < 0:
            continue
        buf = model.Buffers(buf_idx)
        data = buf.DataAsNumpy()
        if data is None or len(data) == 0:
            continue  # skip non-constant tensors

        # FlatBuffer stores data inline; grab offset
        data_vec = buf._tab.Offset(4)
        if data_vec == 0:
            continue

        abs_off = buf._tab.Vector(data_vec)
        size = len(data)

        ranges.append((abs_off, abs_off + size))

    # Merge overlapping / adjacent ranges
    if not ranges:
        return []

    ranges.sort()
    merged = [ranges[0]]
    for start, end in ranges[1:]:
        last_s, last_e = merged[-1]
        if start <= last_e:
            merged[-1] = (last_s, max(last_e, end))
        else:
            merged.append((start, end))

    return merged


def find_weight_diffs(base: bytes, target: bytes, weight_ranges: List[Tuple[int,int]],
                      merge_gap: int, min_chunk: int):
    """Diff only inside weight ranges."""
    all_diffs = []

    for (ws, we) in weight_ranges:
        bs = max(0, ws)
        be = min(len(base), we)
        ts = max(0, ws)
        te = min(len(target), we)

        if bs >= be or ts >= te:
            continue

        segment_diffs = find_diffs(
            base[bs:be],
            target[ts:te],
            merge_gap=merge_gap
        )

        # The returned offsets are relative to the segment; adjust them
        for off, data in segment_diffs:
            all_diffs.append((ws + off, data))

    # Now coalesce small diffs globally
    if not all_diffs:
        return []

    all_diffs.sort()
    coalesced = []
    for off, data in all_diffs:
        if coalesced:
            p_off, p_data = coalesced[-1]
            p_end = p_off + len(p_data)

            if len(data) < min_chunk and off <= p_end + merge_gap:
                gap = target[p_end:off]
                coalesced[-1] = (p_off, p_data + gap + data)
                continue

        coalesced.append((off, data))

    return coalesced


# ---------------------------------------------------------------------------
# Existing helper functions (unchanged)
# ---------------------------------------------------------------------------

def rle_encode(data: bytes) -> bytes:
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


def find_diffs(base: bytes, target: bytes, merge_gap: int = 16):
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
    if len(target) > n:
        diffs.append([n, bytearray(target[n:])])

    if not diffs:
        return []

    merged = [diffs[0]]
    for off, data in diffs[1:]:
        prev_off, prev_data = merged[-1]
        prev_end = prev_off + len(prev_data)
        if off - prev_end <= merge_gap:
            merged[-1][1].extend(target[prev_end:off])
            merged[-1][1].extend(data)
        else:
            merged.append([off, data])
    return [(off, bytes(data)) for off, data in merged]


def tlv(tag: int, payload: bytes) -> bytes:
    if len(payload) > 255:
        raise ValueError("TLV payload too large")
    return struct.pack("<BB", tag & 0xFF, len(payload)) + payload


# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="TinyMLDelta weight-aware patch generator.")
    ap.add_argument("base", help="base .tflite path")
    ap.add_argument("target", help="target .tflite path")
    ap.add_argument("out", help="output patch file path")

    ap.add_argument("--algo", choices=["none","crc32"], default="crc32")
    ap.add_argument("--merge-gap", type=int, default=16)
    ap.add_argument("--min-chunk", type=int, default=8)

    ap.add_argument("--auto-meta", action="store_true")
    ap.add_argument("--arena-factor", type=float, default=1.4)

    ap.add_argument("--req-arena", type=int, default=None)
    ap.add_argument("--tflm-abi", type=int, default=None)
    ap.add_argument("--opset-hash", type=lambda x:int(x,0), default=None)
    ap.add_argument("--io-hash", type=lambda x:int(x,0), default=None)

    args = ap.parse_args()

    base = open(args.base, "rb").read()
    target = open(args.target, "rb").read()

    # ------------ WEIGHT RANGES ------------
    weight_ranges = tflite_weight_ranges(target)
    print(f"[TMD] Weight regions: {weight_ranges}")

    # ------------ WEIGHT-AWARE DIFF ------------
    diffs = find_weight_diffs(
        base, target,
        weight_ranges,
        merge_gap=args.merge_gap,
        min_chunk=args.min_chunk
    )

    # ------------ integrity digests ------------
    if args.algo == "crc32":
        base_chk = struct.pack("<I", zlib.crc32(base) & 0xFFFFFFFF) + b"\x00"*28
        tgt_chk  = struct.pack("<I", zlib.crc32(target) & 0xFFFFFFFF) + b"\x00"*28
        algo = ALGO_CRC32
        chunk_has_crc = 1
    else:
        base_chk = b"\x00"*32
        tgt_chk  = b"\x00"*32
        algo = ALGO_NONE
        chunk_has_crc = 0

    # ------------ metadata TLVs ------------
    auto_req_arena = auto_abi = auto_opset = auto_io = None
    if args.auto_meta:
        try:
            auto_abi, auto_opset, auto_io, auto_req_arena = compute_from_tflite(
                args.target,
                None if args.arena_factor <= 0 else float(args.arena_factor)
            )
        except Exception as e:
            print(f"[TinyMLDelta] Auto-metadata unavailable: {e}")

    req_arena = args.req_arena if args.req_arena is not None else (auto_req_arena or 0)
    tflm_abi  = args.tflm_abi  if args.tflm_abi  is not None else (auto_abi or 0)
    opset     = args.opset_hash if args.opset_hash is not None else (auto_opset or 0)
    io_hash   = args.io_hash   if args.io_hash   is not None else (auto_io or 0)

    meta = bytearray()
    if req_arena: meta += tlv(TMD_META_REQ_ARENA_BYTES, struct.pack("<I", req_arena))
    if tflm_abi: meta += tlv(TMD_META_TFLM_ABI, struct.pack("<H", tflm_abi))
    if opset: meta += tlv(TMD_META_OPSET_HASH, struct.pack("<I", opset))
    if io_hash: meta += tlv(TMD_META_IO_HASH, struct.pack("<I", io_hash))

    # ------------ encode chunks ------------
    chunks = []
    for off, raw in diffs:
        rle = rle_encode(raw)
        if len(rle) < len(raw):
            chunks.append((off, ENC_RLE, rle))
        else:
            chunks.append((off, ENC_RAW, raw))

    # ------------ write patch file ------------
    with open(args.out, "wb") as out:
        hdr = struct.pack(
            HDR_FMT, 1, algo, len(chunks),
            len(base), len(target),
            base_chk, tgt_chk, len(meta), 0
        )
        out.write(hdr)
        if meta:
            out.write(meta)

        for off, enc, data in chunks:
            ch = struct.pack(CHUNK_FMT, off, len(data), enc, chunk_has_crc)
            out.write(ch)
            if chunk_has_crc:
                out.write(struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF))
            out.write(data)

    print(f"TinyMLDelta WEIGHT-AWARE patch written: {args.out}")
    print(f"Chunks: {len(chunks)}   Encoded bytes: {sum(len(c[2]) for c in chunks)}")
    print(f"Metadata bytes: {len(meta)}")


if __name__ == "__main__":
    main()
