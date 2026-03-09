/*
 * msgpack.h — Minimal MsgPack encoder / streaming decoder
 *
 * Handles only the subset used by this application:
 *   Encode: fixarray/array32, fixstr/str8/str16, fixint/uint8/uint16/uint32, nil
 *   Decode: same, plus bool, array16
 *
 * Header-only; no external dependencies beyond <cstdint>, <string>, <vector>.
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// Decoded value
// =============================================================================

struct MpVal {
    enum Type { NIL, BOOL, UINT, STR, ARRAY } type = NIL;
    bool               bval = false;
    uint64_t           uval = 0;
    std::string        sval;
    std::vector<MpVal> aval;

    bool is_nil()   const { return type == NIL; }
    bool is_uint()  const { return type == UINT; }
    bool is_str()   const { return type == STR; }
    bool is_array() const { return type == ARRAY; }
    uint64_t           as_uint()  const { return uval; }
    const std::string& as_str()   const { return sval; }
    const std::vector<MpVal>& as_array() const { return aval; }
};

// =============================================================================
// Decoder
// =============================================================================

// Decode one MsgPack value from data[pos..len].
// Returns bytes consumed (>0), 0 if incomplete, -1 on error.
static inline int mp_decode(const uint8_t *data, size_t len, size_t pos, MpVal &out) {
    if (pos >= len) return 0;
    size_t start = pos;
    uint8_t b = data[pos++];

    if (b <= 0x7F) {                                   // positive fixint
        out = {}; out.type = MpVal::UINT; out.uval = b;
        return (int)(pos - start);
    }
    if (b >= 0x90 && b <= 0x9F) {                      // fixarray
        out = {}; out.type = MpVal::ARRAY;
        uint32_t n = b & 0x0Fu;
        for (uint32_t i = 0; i < n; ++i) {
            MpVal elem;
            int r = mp_decode(data, len, pos, elem);
            if (r <= 0) return r;
            pos += (size_t)r;
            out.aval.push_back(std::move(elem));
        }
        return (int)(pos - start);
    }
    if (b >= 0xA0 && b <= 0xBF) {                      // fixstr
        uint32_t n = b & 0x1Fu;
        if (pos + n > len) return 0;
        out = {}; out.type = MpVal::STR;
        out.sval.assign((const char *)data + pos, n);
        pos += n;
        return (int)(pos - start);
    }
    if (b == 0xC0) { out = {}; return 1; }             // nil
    if (b == 0xC2) { out = {}; out.type = MpVal::BOOL; out.bval = false; return 1; }
    if (b == 0xC3) { out = {}; out.type = MpVal::BOOL; out.bval = true;  return 1; }
    if (b == 0xCC) {                                   // uint8
        if (pos >= len) return 0;
        out = {}; out.type = MpVal::UINT; out.uval = data[pos++];
        return (int)(pos - start);
    }
    if (b == 0xCD) {                                   // uint16
        if (pos + 2 > len) return 0;
        out = {}; out.type = MpVal::UINT;
        out.uval = ((uint64_t)data[pos] << 8) | data[pos + 1];
        pos += 2; return (int)(pos - start);
    }
    if (b == 0xCE) {                                   // uint32
        if (pos + 4 > len) return 0;
        out = {}; out.type = MpVal::UINT;
        out.uval = ((uint64_t)data[pos    ] << 24) | ((uint64_t)data[pos + 1] << 16)
                 | ((uint64_t)data[pos + 2] <<  8) |            data[pos + 3];
        pos += 4; return (int)(pos - start);
    }
    if (b == 0xD9) {                                   // str8
        if (pos >= len) return 0;
        uint32_t n = data[pos++];
        if (pos + n > len) return 0;
        out = {}; out.type = MpVal::STR;
        out.sval.assign((const char *)data + pos, n);
        pos += n; return (int)(pos - start);
    }
    if (b == 0xDA) {                                   // str16
        if (pos + 2 > len) return 0;
        uint32_t n = ((uint32_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (pos + n > len) return 0;
        out = {}; out.type = MpVal::STR;
        out.sval.assign((const char *)data + pos, n);
        pos += n; return (int)(pos - start);
    }
    if (b == 0xDC) {                                   // array16
        if (pos + 2 > len) return 0;
        uint32_t n = ((uint32_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
        out = {}; out.type = MpVal::ARRAY;
        for (uint32_t i = 0; i < n; ++i) {
            MpVal elem;
            int r = mp_decode(data, len, pos, elem);
            if (r <= 0) return r;
            pos += (size_t)r;
            out.aval.push_back(std::move(elem));
        }
        return (int)(pos - start);
    }
    return -1;  // unhandled type byte
}

// Try to decode one value from buf, consuming it on success.
static inline bool mp_try_decode(std::vector<uint8_t> &buf, MpVal &out) {
    if (buf.empty()) return false;
    int r = mp_decode(buf.data(), buf.size(), 0, out);
    if (r <= 0) return false;
    buf.erase(buf.begin(), buf.begin() + r);
    return true;
}

// =============================================================================
// Encoder helpers — append MsgPack bytes to buf.
// =============================================================================

static inline void mp_nil(std::vector<uint8_t> &b) { b.push_back(0xC0); }

static inline void mp_uint(std::vector<uint8_t> &b, uint32_t v) {
    if      (v <= 0x7Fu)   { b.push_back((uint8_t)v); }
    else if (v <= 0xFFu)   { b.push_back(0xCC); b.push_back((uint8_t)v); }
    else if (v <= 0xFFFFu) { b.push_back(0xCD); b.push_back(v >> 8); b.push_back(v & 0xFF); }
    else { b.push_back(0xCE);
           b.push_back(v >> 24); b.push_back(v >> 16);
           b.push_back(v >>  8); b.push_back(v & 0xFF); }
}

static inline void mp_str(std::vector<uint8_t> &b, const std::string &s) {
    size_t n = s.size();
    if      (n <= 31u)   { b.push_back(0xA0u | (uint8_t)n); }
    else if (n <= 255u)  { b.push_back(0xD9); b.push_back((uint8_t)n); }
    else                 { b.push_back(0xDA); b.push_back(n >> 8); b.push_back(n & 0xFF); }
    b.insert(b.end(), s.begin(), s.end());
}

static inline void mp_arr(std::vector<uint8_t> &b, uint32_t n) {
    if (n <= 15u) { b.push_back(0x90u | (uint8_t)n); }
    else { b.push_back(0xDD);
           b.push_back(n >> 24); b.push_back(n >> 16);
           b.push_back(n >>  8); b.push_back(n & 0xFF); }
}

// Build a complete RPC response: [1, msgid, nil_or_error, result_str]
static inline std::vector<uint8_t> mp_response(uint32_t msgid,
                                                const char *error,
                                                const std::string &result) {
    std::vector<uint8_t> pkt;
    mp_arr(pkt, 4);
    mp_uint(pkt, 1);           // RESPONSE
    mp_uint(pkt, msgid);
    if (error) mp_str(pkt, error); else mp_nil(pkt);
    mp_str(pkt, result);
    return pkt;
}

// Build a complete RPC request: [0, msgid, method, [arg]]
static inline std::vector<uint8_t> mp_request(uint32_t msgid,
                                               const std::string &method,
                                               const std::string &arg) {
    std::vector<uint8_t> pkt;
    mp_arr(pkt, 4);
    mp_uint(pkt, 0);           // REQUEST
    mp_uint(pkt, msgid);
    mp_str(pkt, method);
    mp_arr(pkt, 1);
    mp_str(pkt, arg);
    return pkt;
}
