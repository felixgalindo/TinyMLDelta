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


def _lz4_block_decode(src: bytes) -> bytes:
    """Decode a raw LZ4 block (mirrors the C runtime's tmd_lz4_block_decode)."""
    out = bytearray()
    sp, n = 0, len(src)
    while sp < n:
        token = src[sp]; sp += 1
        litlen = token >> 4
        if litlen == 15:
            while True:
                b = src[sp]; sp += 1; litlen += b
                if b != 255:
                    break
        out += src[sp:sp + litlen]; sp += litlen
        if sp >= n:
            break
        offset = src[sp] | (src[sp + 1] << 8); sp += 2
        matchlen = token & 0x0F
        if matchlen == 15:
            while True:
                b = src[sp]; sp += 1; matchlen += b
                if b != 255:
                    break
        matchlen += 4
        mp = len(out) - offset
        for _ in range(matchlen):
            out.append(out[mp]); mp += 1
    return bytes(out)


def _apply_copyadd(base, patch, nch, tlen, mlen, has_crc) -> bytes:
    """Apply a COPY/ADD opcode-stream patch (mirrors the C runtime)."""
    out = bytearray()
    p = _HS + mlen
    for _ in range(nch):
        op = patch[p]
        p += 1
        if op == 1:  # COPY src_off,len from base
            src, length = struct.unpack("<II", patch[p:p + 8])
            p += 8
            out += base[src:src + length]
        else:        # ADD enc,len [crc] data
            enc, length = struct.unpack("<BI", patch[p:p + 5])
            p += 5
            crc = None
            if has_crc:
                crc = struct.unpack("<I", patch[p:p + 4])[0]
                p += 4
            data = patch[p:p + length]
            p += length
            if crc is not None and (zlib.crc32(data) & 0xFFFFFFFF) != crc:
                raise ValueError("ADD CRC mismatch")
            if enc == 1:  # RLE
                dec = bytearray()
                i = 0
                while i < len(data):
                    count = data[i] or 256
                    dec += bytes([data[i + 1]]) * count
                    i += 2
                data = bytes(dec)
            out += data
    if len(out) != tlen:
        raise ValueError(f"COPY/ADD produced {len(out)} != target_len {tlen}")
    return bytes(out)


def apply_patch(base: bytes, patch: bytes) -> bytes:
    """Apply a .tmd patch to `base` and return the reconstructed target bytes."""
    v, algo, nch, blen, tlen, _bchk, _tchk, mlen, _flags = struct.unpack(
        HDR_FMT, patch[:_HS]
    )
    if v != 1:
        raise ValueError(f"unsupported version {v}")
    if len(base) != blen:
        raise ValueError(f"base length {len(base)} != header base_len {blen}")

    if _flags & 0x0001:  # FLAG_COPYADD: opcode stream, not positional chunks
        return _apply_copyadd(base, patch, nch, tlen, mlen, algo == 1)

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
        elif enc == 2:  # LZ4 raw block (mirrors the C decoder)
            payload = _lz4_block_decode(payload)
        out[off:off + len(payload)] = payload

    return bytes(out)
