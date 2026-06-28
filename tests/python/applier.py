"""Pure-Python reference applier for .tmd patches (test oracle).

Mirrors the C runtime semantics: copy base, grow/shrink to target_len, then
apply each chunk (verifying CRC, decoding RLE) at its offset. Used by the
roundtrip tests to assert patch(base, target) reconstructs target byte-exactly.

License: Apache-2.0
"""
import struct
import zlib

HDR_FMT = "<BBHII32s32sHH"
CHUNK_FMT = "<IHBb"
_HS = struct.calcsize(HDR_FMT)
_CS = struct.calcsize(CHUNK_FMT)


def apply_patch(base: bytes, patch: bytes) -> bytes:
    """Apply a .tmd patch to `base` and return the reconstructed target bytes."""
    v, algo, nch, blen, tlen, _bchk, _tchk, mlen, _flags = struct.unpack(
        HDR_FMT, patch[:_HS]
    )
    if v != 1:
        raise ValueError(f"unsupported version {v}")
    if len(base) != blen:
        raise ValueError(f"base length {len(base)} != header base_len {blen}")

    out = bytearray(base)
    if tlen > len(out):
        out.extend(b"\x00" * (tlen - len(out)))
    else:
        del out[tlen:]

    p = _HS + mlen
    for _ in range(nch):
        off, ln, enc, has_crc = struct.unpack(CHUNK_FMT, patch[p:p + _CS])
        p += _CS
        crc = None
        if has_crc:
            crc = struct.unpack("<I", patch[p:p + 4])[0]
            p += 4
        payload = patch[p:p + ln]
        p += ln
        if crc is not None and (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
            raise ValueError(f"CRC mismatch at offset {off}")
        if enc == 1:  # RLE: sequence of [count][byte], count 0 => 256
            dec = bytearray()
            i = 0
            while i < len(payload):
                count = payload[i] or 256
                dec += bytes([payload[i + 1]]) * count
                i += 2
            payload = bytes(dec)
        out[off:off + len(payload)] = payload

    return bytes(out)
