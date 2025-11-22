/**
 * @file tinymldelta_core.c
 * @brief TinyMLDelta — Generic patch applier using platform ports.
 * @author Felix Galindo
 * @license Apache-2.0
 */

#include <string.h>
#include "tinymldelta.h"
#include "tinymldelta_config.h"
#include "tinymldelta_internal.h"
#include "tinymldelta_ports.h"

#if TMD_FEAT_LOG
#define TMD_LOG(...) \
  do { \
    const tmd_ports_t* P = tmd_ports(); \
    if (P && P->log) P->log(__VA_ARGS__); \
  } while (0)
#else
#define TMD_LOG(...) do {} while (0)
#endif

#define TMD_JOURNAL_MAGIC 0x544D4450u /* 'TMDP' */

/**
 * @brief In-memory view of parsed metadata TLVs.
 */
typedef struct {
  uint32_t req_arena_bytes;
  uint16_t tflm_abi;
  uint32_t opset_hash;
  uint32_t io_hash;
} tmd_meta_state_t;

/**
 * @brief Simple RLE decode: [count][byte], count==0 => 256.
 *
 * Format:
 *   [c0][v0][c1][v1]...
 *
 * Where:
 *   - c == 0 encodes run length 256.
 *   - Otherwise, run length == c (1..255).
 *
 * @param in       Pointer to encoded input buffer.
 * @param in_len   Length of encoded input.
 * @param out      Destination buffer for decoded bytes.
 * @param out_cap  Capacity of @p out in bytes.
 * @param out_len  Receives decoded length on success.
 *
 * @return 0 on success, -1 on error (e.g., overflow).
 */
static int tmd_rle_decode(const uint8_t* in, uint16_t in_len,
                          uint8_t* out, uint32_t out_cap,
                          uint32_t* out_len) {
#if !TMD_FEAT_RLE
  (void)in; (void)in_len; (void)out; (void)out_cap; (void)out_len;
  return -1;
#else
  uint32_t o = 0;
  uint16_t i = 0;

  TMD_LOG("TinyMLDelta: RLE decode start (in_len=%u)\n", (unsigned)in_len);

  while (i + 1 <= in_len) {
    uint8_t count = in[i++];
    uint8_t val   = in[i++];
    uint32_t run = (count == 0) ? 256u : (uint32_t)count;

    if (o + run > out_cap) {
      TMD_LOG("TinyMLDelta: RLE overflow (o=%lu run=%lu cap=%lu)\n",
              (unsigned long)o,
              (unsigned long)run,
              (unsigned long)out_cap);
      return -1;
    }
    memset(out + o, val, run);
    o += run;
  }
  *out_len = o;

  TMD_LOG("TinyMLDelta: RLE decode done (out_len=%lu)\n", (unsigned long)o);
  return 0;
#endif
}

/**
 * @brief Parse metadata TLV block from the patch header.
 *
 * Core understands the standard TLVs; vendor TLVs are ignored.
 *
 * @param buf       Pointer to TLV blob.
 * @param meta_len  Length of TLV blob in bytes.
 * @param meta      Output: parsed metadata state.
 *
 * @return TMD_STATUS_OK on success, error status otherwise.
 */
static tmd_status_t tmd_parse_meta(const uint8_t* buf,
                                   uint16_t meta_len,
                                   tmd_meta_state_t* meta) {
  memset(meta, 0, sizeof(*meta));
  uint16_t off = 0;

  TMD_LOG("TinyMLDelta: parsing meta TLVs (meta_len=%u)\n",
          (unsigned)meta_len);

  while (off + sizeof(tmd_meta_tlv_t) <= meta_len) {
    const tmd_meta_tlv_t* tlv = (const tmd_meta_tlv_t*)(buf + off);
    uint16_t val_off = off + sizeof(tlv->tag) + sizeof(tlv->len);
    uint16_t avail   = (uint16_t)(meta_len - val_off);

    if (tlv->len > avail) {
      TMD_LOG("TinyMLDelta: TLV length exceed (tag=%u len=%u avail=%u)\n",
              (unsigned)tlv->tag,
              (unsigned)tlv->len,
              (unsigned)avail);
      return TMD_STATUS_ERR_HDR;
    }

    const uint8_t* val = buf + val_off;
    switch (tlv->tag) {
      case TMD_META_REQ_ARENA_BYTES:
        if (tlv->len == 4) {
          meta->req_arena_bytes = (uint32_t)val[0] |
                                  ((uint32_t)val[1] << 8) |
                                  ((uint32_t)val[2] << 16) |
                                  ((uint32_t)val[3] << 24);
          TMD_LOG("TinyMLDelta: meta.req_arena_bytes=%lu\n",
                  (unsigned long)meta->req_arena_bytes);
        }
        break;
      case TMD_META_TFLM_ABI:
        if (tlv->len == 2) {
          meta->tflm_abi = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
          TMD_LOG("TinyMLDelta: meta.tflm_abi=%u\n",
                  (unsigned)meta->tflm_abi);
        }
        break;
      case TMD_META_OPSET_HASH:
        if (tlv->len == 4) {
          meta->opset_hash = (uint32_t)val[0] |
                             ((uint32_t)val[1] << 8) |
                             ((uint32_t)val[2] << 16) |
                             ((uint32_t)val[3] << 24);
          TMD_LOG("TinyMLDelta: meta.opset_hash=0x%08lx\n",
                  (unsigned long)meta->opset_hash);
        }
        break;
      case TMD_META_IO_HASH:
        if (tlv->len == 4) {
          meta->io_hash = (uint32_t)val[0] |
                          ((uint32_t)val[1] << 8) |
                          ((uint32_t)val[2] << 16) |
                          ((uint32_t)val[3] << 24);
          TMD_LOG("TinyMLDelta: meta.io_hash=0x%08lx\n",
                  (unsigned long)meta->io_hash);
        }
        break;
      default:
        /* Vendor / unknown TLVs are ignored by core. */
        TMD_LOG("TinyMLDelta: meta TLV ignored (tag=%u len=%u)\n",
                (unsigned)tlv->tag,
                (unsigned)tlv->len);
        break;
    }

    off = (uint16_t)(val_off + tlv->len);
  }

  return TMD_STATUS_OK;
}

/**
 * @brief Enforce firmware guardrails based on metadata TLVs.
 *
 * Checks:
 *   - Required arena <= firmware's configured arena.
 *   - Required TFLM ABI <= firmware ABI.
 *   - Optional opset hash match.
 *   - Optional IO hash match (if enabled).
 *
 * @param meta Parsed metadata state.
 *
 * @return TMD_STATUS_OK if all guardrails pass, error otherwise.
 */
static tmd_status_t tmd_check_guardrails(const tmd_meta_state_t* meta) {
  TMD_LOG("TinyMLDelta: guardrail check\n");
  TMD_LOG("TinyMLDelta:  req_arena_bytes=%lu firmware=%lu\n",
          (unsigned long)meta->req_arena_bytes,
          (unsigned long)TMD_FIRMWARE_ARENA_BYTES);
  TMD_LOG("TinyMLDelta:  tflm_abi=%u firmware=%u\n",
          (unsigned)meta->tflm_abi,
          (unsigned)TMD_FIRMWARE_TFLM_ABI);
  TMD_LOG("TinyMLDelta:  opset_hash=0x%08lx firmware=0x%08lx\n",
          (unsigned long)meta->opset_hash,
          (unsigned long)TMD_FIRMWARE_OPSET_HASH);
#if TMD_ENFORCE_IO_HASH
  TMD_LOG("TinyMLDelta:  io_hash=0x%08lx firmware=0x%08lx\n",
          (unsigned long)meta->io_hash,
          (unsigned long)TMD_FIRMWARE_IO_HASH);
#endif

  if (meta->req_arena_bytes &&
      meta->req_arena_bytes > TMD_FIRMWARE_ARENA_BYTES) {
    TMD_LOG("TinyMLDelta: arena guardrail fail (%lu > %lu)\n",
            (unsigned long)meta->req_arena_bytes,
            (unsigned long)TMD_FIRMWARE_ARENA_BYTES);
    return TMD_STATUS_ERR_GUARDRAIL;
  }
  if (meta->tflm_abi &&
      meta->tflm_abi > TMD_FIRMWARE_TFLM_ABI) {
    TMD_LOG("TinyMLDelta: TFLM ABI guardrail fail (%u > %u)\n",
            (unsigned)meta->tflm_abi,
            (unsigned)TMD_FIRMWARE_TFLM_ABI);
    return TMD_STATUS_ERR_GUARDRAIL;
  }
  if (TMD_FIRMWARE_OPSET_HASH &&
      meta->opset_hash &&
      meta->opset_hash != TMD_FIRMWARE_OPSET_HASH) {
    TMD_LOG("TinyMLDelta: opset hash mismatch\n");
    return TMD_STATUS_ERR_GUARDRAIL;
  }
#if TMD_ENFORCE_IO_HASH
  if (TMD_FIRMWARE_IO_HASH &&
      meta->io_hash &&
      meta->io_hash != TMD_FIRMWARE_IO_HASH) {
    TMD_LOG("TinyMLDelta: IO hash mismatch\n");
    return TMD_STATUS_ERR_GUARDRAIL;
  }
#endif
  return TMD_STATUS_OK;
}

/**
 * @brief Copy an entire slot from src to dst using the ports interface.
 *
 * This is used to clone the currently-active slot into the inactive slot
 * before applying chunk-level modifications.
 *
 * @param P   Ports vtable.
 * @param src Source slot.
 * @param dst Destination slot.
 *
 * @return TMD_STATUS_OK on success, error code otherwise.
 */
static tmd_status_t tmd_copy_slot(const tmd_ports_t* P,
                                  const tmd_slot_t* src,
                                  const tmd_slot_t* dst) {
  uint8_t buf[TMD_SCRATCH_SZ];
  uint32_t remaining = src->size;
  uint32_t src_off = 0;
  uint32_t dst_off = 0;

  TMD_LOG("TinyMLDelta: copy_slot src=0x%08lx dst=0x%08lx size=%lu\n",
          (unsigned long)src->addr,
          (unsigned long)dst->addr,
          (unsigned long)src->size);

  /* Erase destination range. */
  if (!P->flash_erase(dst->addr, dst->size)) {
    TMD_LOG("TinyMLDelta: flash_erase failed @0x%08lx size=%lu\n",
            (unsigned long)dst->addr,
            (unsigned long)dst->size);
    return TMD_STATUS_ERR_FLASH;
  }

  while (remaining > 0) {
    uint32_t chunk = (remaining > TMD_SCRATCH_SZ) ? TMD_SCRATCH_SZ : remaining;

    TMD_LOG("TinyMLDelta:  copy chunk src_off=%lu dst_off=%lu len=%lu\n",
            (unsigned long)src_off,
            (unsigned long)dst_off,
            (unsigned long)chunk);

    if (!P->flash_read(src->addr + src_off, buf, chunk)) {
      TMD_LOG("TinyMLDelta: flash_read failed @0x%08lx len=%lu\n",
              (unsigned long)(src->addr + src_off),
              (unsigned long)chunk);
      return TMD_STATUS_ERR_FLASH;
    }
    if (!P->flash_write(dst->addr + dst_off, buf, chunk)) {
      TMD_LOG("TinyMLDelta: flash_write failed @0x%08lx len=%lu\n",
              (unsigned long)(dst->addr + dst_off),
              (unsigned long)chunk);
      return TMD_STATUS_ERR_FLASH;
    }
    remaining -= chunk;
    src_off   += chunk;
    dst_off   += chunk;
  }

  TMD_LOG("TinyMLDelta: copy_slot done\n");
  return TMD_STATUS_OK;
}

/**
 * @brief Apply a TinyMLDelta patch already resident in memory.
 *
 * Typical flow:
 *   1. Parse and validate header.
 *   2. Parse metadata TLVs and enforce guardrails.
 *   3. Clone active slot into inactive slot.
 *   4. Apply each chunk into the inactive slot.
 *   5. Flip active slot to the updated one.
 *
 * @param patch      Pointer to patch bytes.
 * @param patch_len  Length of patch in bytes.
 *
 * @return TMD_STATUS_OK on success, error code otherwise.
 */
tmd_status_t tmd_apply_patch_from_memory(const uint8_t* patch, size_t patch_len) {
  const tmd_ports_t*   P = tmd_ports();
  const tmd_layout_t*  L = tmd_layout();
  if (!P || !L || !patch || patch_len < sizeof(tmd_hdr_t)) {
    TMD_LOG("TinyMLDelta: invalid params (P=%p L=%p patch=%p len=%lu)\n",
            (const void*)P,
            (const void*)L,
            (const void*)patch,
            (unsigned long)patch_len);
    return TMD_STATUS_ERR_PARAM;
  }

  const tmd_hdr_t* hdr = (const tmd_hdr_t*)patch;

  /* Patch header debug. */
  TMD_LOG("TinyMLDelta: ---- Patch Header ----\n");
  TMD_LOG("TinyMLDelta: v=%u algo=%u chunks_n=%u\n",
          (unsigned)hdr->v,
          (unsigned)hdr->algo,
          (unsigned)hdr->chunks_n);
  TMD_LOG("TinyMLDelta: base_len=%lu target_len=%lu\n",
          (unsigned long)hdr->base_len,
          (unsigned long)hdr->target_len);
  TMD_LOG("TinyMLDelta: meta_len=%u flags=0x%04x\n",
          (unsigned)hdr->meta_len,
          (unsigned)hdr->flags);

  if (hdr->v != 1) {
    TMD_LOG("TinyMLDelta: unsupported patch version %u\n",
            (unsigned)hdr->v);
    return TMD_STATUS_ERR_HDR;
  }

#if TMD_USE_CRC32
  if (hdr->algo != 1) {
    TMD_LOG("TinyMLDelta: algo=%u not supported (expected CRC32=1)\n",
            (unsigned)hdr->algo);
    return TMD_STATUS_ERR_UNSUPPORTED;
  }
#else
  (void)hdr;
#endif

  size_t off = sizeof(tmd_hdr_t);
  if (off + hdr->meta_len > patch_len) {
    TMD_LOG("TinyMLDelta: meta_len out of range (off=%lu meta_len=%u len=%lu)\n",
            (unsigned long)off,
            (unsigned)hdr->meta_len,
            (unsigned long)patch_len);
    return TMD_STATUS_ERR_HDR;
  }

  /* Parse metadata TLVs. */
  tmd_meta_state_t meta;
  tmd_status_t st = tmd_parse_meta(patch + off, hdr->meta_len, &meta);
  if (st != TMD_STATUS_OK) {
    TMD_LOG("TinyMLDelta: tmd_parse_meta failed (%d)\n", (int)st);
    return st;
  }

  /* Guardrail checks. */
  st = tmd_check_guardrails(&meta);
  if (st != TMD_STATUS_OK) {
    TMD_LOG("TinyMLDelta: guardrail check failed (%d)\n", (int)st);
    return st;
  }

  off += hdr->meta_len;

  uint8_t active   = P->get_active_slot();
  uint8_t inactive = (active == 0) ? 1 : 0;
  const tmd_slot_t* slot_src = (active == 0) ? &L->slotA : &L->slotB;
  const tmd_slot_t* slot_dst = (inactive == 0) ? &L->slotA : &L->slotB;

  TMD_LOG("TinyMLDelta: active slot=%u inactive=%u\n",
          (unsigned)active,
          (unsigned)inactive);
  TMD_LOG("TinyMLDelta: slotA addr=0x%08lx size=%lu\n",
          (unsigned long)L->slotA.addr,
          (unsigned long)L->slotA.size);
  TMD_LOG("TinyMLDelta: slotB addr=0x%08lx size=%lu\n",
          (unsigned long)L->slotB.addr,
          (unsigned long)L->slotB.size);

  if (slot_src->size != slot_dst->size) {
    TMD_LOG("TinyMLDelta: slot size mismatch (src=%lu dst=%lu)\n",
            (unsigned long)slot_src->size,
            (unsigned long)slot_dst->size);
    return TMD_STATUS_ERR_PARAM;
  }

  /* Copy active slot to inactive slot. */
  st = tmd_copy_slot(P, slot_src, slot_dst);
  if (st != TMD_STATUS_OK) {
    TMD_LOG("TinyMLDelta: tmd_copy_slot failed (%d)\n", (int)st);
    return st;
  }

#if TMD_FEAT_JOURNAL
  tmd_journal_t j = {0};
  if (!P->journal_read(&j) || j.magic != TMD_JOURNAL_MAGIC) {
    memset(&j, 0, sizeof(j));
    j.magic = TMD_JOURNAL_MAGIC;
    j.patch_id = 0; /* local-only; could be derived from header digest */
    j.next_chunk_idx = 0;
    j.target_slot = inactive;
    TMD_LOG("TinyMLDelta: journal init (target_slot=%u)\n",
            (unsigned)inactive);
  } else {
    TMD_LOG("TinyMLDelta: journal resume (next_chunk=%lu target_slot=%u)\n",
            (unsigned long)j.next_chunk_idx,
            (unsigned)j.target_slot);
  }
#else
  uint32_t next_chunk_idx = 0;
  (void)next_chunk_idx;
#endif

  uint8_t scratch[TMD_SCRATCH_SZ];

  /* Apply chunks. */
  for (uint32_t idx = 0; idx < hdr->chunks_n; ++idx) {
    if (off + sizeof(tmd_chunk_hdr_t) > patch_len) {
      TMD_LOG("TinyMLDelta: not enough data for chunk hdr idx=%lu\n",
              (unsigned long)idx);
      return TMD_STATUS_ERR_HDR;
    }
    const tmd_chunk_hdr_t* ch = (const tmd_chunk_hdr_t*)(patch + off);
    off += sizeof(tmd_chunk_hdr_t);

    TMD_LOG("TinyMLDelta: chunk[%lu]: off=%lu len=%u enc=%u has_crc=%u\n",
            (unsigned long)idx,
            (unsigned long)ch->off,
            (unsigned)ch->len,
            (unsigned)ch->enc,
            (unsigned)ch->has_crc);

    uint32_t crc_val = 0;
#if TMD_FEAT_CRC32
    if (ch->has_crc) {
      if (off + 4 > patch_len) {
        TMD_LOG("TinyMLDelta: not enough data for chunk CRC idx=%lu\n",
                (unsigned long)idx);
        return TMD_STATUS_ERR_HDR;
      }
      crc_val = (uint32_t)patch[off] |
                ((uint32_t)patch[off+1] << 8) |
                ((uint32_t)patch[off+2] << 16) |
                ((uint32_t)patch[off+3] << 24);
      off += 4;

      TMD_LOG("TinyMLDelta:  chunk[%lu] file_crc=0x%08lx\n",
              (unsigned long)idx,
              (unsigned long)crc_val);
    }
#endif
    if (off + ch->len > patch_len) {
      TMD_LOG("TinyMLDelta: chunk payload exceeds patch len idx=%lu\n",
              (unsigned long)idx);
      return TMD_STATUS_ERR_HDR;
    }
    const uint8_t* enc_data = patch + off;
    off += ch->len;

#if TMD_FEAT_CRC32
    if (ch->has_crc && P->crc32) {
      uint32_t got = P->crc32(enc_data, ch->len);
      if (got != crc_val) {
        TMD_LOG("TinyMLDelta: chunk CRC mismatch idx=%lu got=0x%08lx exp=0x%08lx\n",
                (unsigned long)idx,
                (unsigned long)got,
                (unsigned long)crc_val);
        return TMD_STATUS_ERR_INTEGRITY;
      }
    }
#endif

    const uint8_t* data = NULL;
    uint32_t data_len = 0;

    if (ch->enc == 0) { /* RAW */
      data = enc_data;
      data_len = ch->len;
      TMD_LOG("TinyMLDelta:  chunk[%lu] RAW len=%lu\n",
              (unsigned long)idx,
              (unsigned long)data_len);
    } else if (ch->enc == 1) { /* RLE */
      if (tmd_rle_decode(enc_data, ch->len,
                         scratch, sizeof(scratch),
                         &data_len) != 0) {
        TMD_LOG("TinyMLDelta: RLE decode failed idx=%lu\n",
                (unsigned long)idx);
        return TMD_STATUS_ERR_HDR;
      }
      data = scratch;
      TMD_LOG("TinyMLDelta:  chunk[%lu] RLE decoded len=%lu\n",
              (unsigned long)idx,
              (unsigned long)data_len);
    } else {
      TMD_LOG("TinyMLDelta: unsupported encoding %u\n", (unsigned)ch->enc);
      return TMD_STATUS_ERR_UNSUPPORTED;
    }

    if (ch->off + data_len > slot_dst->size) {
      TMD_LOG("TinyMLDelta: chunk out of range (off=%lu,len=%lu,size=%lu)\n",
              (unsigned long)ch->off,
              (unsigned long)data_len,
              (unsigned long)slot_dst->size);
      return TMD_STATUS_ERR_PARAM;
    }

    uint32_t addr = slot_dst->addr + ch->off;
    TMD_LOG("TinyMLDelta:  flash_write addr=0x%08lx len=%lu\n",
            (unsigned long)addr,
            (unsigned long)data_len);

    if (!P->flash_write(addr, data, data_len)) {
      TMD_LOG("TinyMLDelta: flash_write failed @0x%08lx len=%lu\n",
              (unsigned long)addr,
              (unsigned long)data_len);
      return TMD_STATUS_ERR_FLASH;
    }

#if TMD_FEAT_JOURNAL
    j.next_chunk_idx = idx + 1;
    P->journal_write(&j);
#endif
  }

#if TMD_FEAT_JOURNAL
  TMD_LOG("TinyMLDelta: clearing journal\n");
  P->journal_clear();
#endif

  if (!P->set_active_slot(inactive)) {
    TMD_LOG("TinyMLDelta: set_active_slot(%u) failed\n",
            (unsigned)inactive);
    return TMD_STATUS_ERR_FLASH;
  }

  TMD_LOG("TinyMLDelta: patch applied OK, new active slot=%u\n",
          (unsigned)inactive);
  return TMD_STATUS_OK;
}
