/**
 * @file tinymldelta_core.c
 * @brief TinyMLDelta — Generic patch applier using platform ports.
 */

#include <string.h>
#include "tinymldelta.h"
#include "tinymldelta_config.h"
#include "tinymldelta_internal.h"
#include "tinymldelta_ports.h"

#if TMD_FEAT_LOG
#define TMD_LOG(...) do { const tmd_ports_t* P = tmd_ports(); if (P && P->log) P->log(__VA_ARGS__); } while (0)
#else
#define TMD_LOG(...) do {} while (0)
#endif

#define TMD_JOURNAL_MAGIC 0x544D4450u /* 'TMDP' */

typedef struct {
  uint32_t req_arena_bytes;
  uint16_t tflm_abi;
  uint32_t opset_hash;
  uint32_t io_hash;
} tmd_meta_state_t;

/* Simple RLE decode: [count][byte], count==0 => 256 */
static int tmd_rle_decode(const uint8_t* in, uint16_t in_len,
                          uint8_t* out, uint32_t out_cap,
                          uint32_t* out_len) {
#if !TMD_FEAT_RLE
  (void)in; (void)in_len; (void)out; (void)out_cap; (void)out_len;
  return -1;
#else
  uint32_t o = 0;
  uint16_t i = 0;
  while (i + 1 <= in_len) {
    uint8_t count = in[i++];
    uint8_t val   = in[i++];
    uint32_t run = (count == 0) ? 256u : (uint32_t)count;
    if (o + run > out_cap) return -1;
    memset(out + o, val, run);
    o += run;
  }
  *out_len = o;
  return 0;
#endif
}

static tmd_status_t tmd_parse_meta(const uint8_t* buf, uint16_t meta_len, tmd_meta_state_t* meta) {
  memset(meta, 0, sizeof(*meta));
  uint16_t off = 0;
  while (off + sizeof(tmd_meta_tlv_t) <= meta_len) {
    const tmd_meta_tlv_t* tlv = (const tmd_meta_tlv_t*)(buf + off);
    uint16_t val_off = off + sizeof(tlv->tag) + sizeof(tlv->len);
    uint16_t avail = meta_len - val_off;
    if (tlv->len > avail) return TMD_STATUS_ERR_HDR;
    const uint8_t* val = buf + val_off;
    switch (tlv->tag) {
      case TMD_META_REQ_ARENA_BYTES:
        if (tlv->len == 4) {
          meta->req_arena_bytes = (uint32_t)val[0] |
                                  ((uint32_t)val[1] << 8) |
                                  ((uint32_t)val[2] << 16) |
                                  ((uint32_t)val[3] << 24);
        }
        break;
      case TMD_META_TFLM_ABI:
        if (tlv->len == 2) {
          meta->tflm_abi = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
        }
        break;
      case TMD_META_OPSET_HASH:
        if (tlv->len == 4) {
          meta->opset_hash = (uint32_t)val[0] |
                             ((uint32_t)val[1] << 8) |
                             ((uint32_t)val[2] << 16) |
                             ((uint32_t)val[3] << 24);
        }
        break;
      case TMD_META_IO_HASH:
        if (tlv->len == 4) {
          meta->io_hash = (uint32_t)val[0] |
                          ((uint32_t)val[1] << 8) |
                          ((uint32_t)val[2] << 16) |
                          ((uint32_t)val[3] << 24);
        }
        break;
      default:
        /* vendor TLVs are ignored by core */
        break;
    }
    off = val_off + tlv->len;
  }
  return TMD_STATUS_OK;
}

static tmd_status_t tmd_check_guardrails(const tmd_meta_state_t* meta) {
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

static tmd_status_t tmd_copy_slot(const tmd_ports_t* P,
                                  const tmd_slot_t* src,
                                  const tmd_slot_t* dst) {
  uint8_t buf[TMD_SCRATCH_SZ];
  uint32_t remaining = src->size;
  uint32_t src_off = 0;
  uint32_t dst_off = 0;

  /* erase destination range */
  if (!P->flash_erase(dst->addr, dst->size)) {
    return TMD_STATUS_ERR_FLASH;
  }

  while (remaining > 0) {
    uint32_t chunk = remaining > TMD_SCRATCH_SZ ? TMD_SCRATCH_SZ : remaining;
    if (!P->flash_read(src->addr + src_off, buf, chunk)) {
      return TMD_STATUS_ERR_FLASH;
    }
    if (!P->flash_write(dst->addr + dst_off, buf, chunk)) {
      return TMD_STATUS_ERR_FLASH;
    }
    remaining -= chunk;
    src_off   += chunk;
    dst_off   += chunk;
  }
  return TMD_STATUS_OK;
}

tmd_status_t tmd_apply_patch_from_memory(const uint8_t* patch, size_t patch_len) {
  const tmd_ports_t*   P = tmd_ports();
  const tmd_layout_t*  L = tmd_layout();
  if (!P || !L || !patch || patch_len < sizeof(tmd_hdr_t)) {
    return TMD_STATUS_ERR_PARAM;
  }

  const tmd_hdr_t* hdr = (const tmd_hdr_t*)patch;
  if (hdr->v != 1) {
    TMD_LOG("TinyMLDelta: unsupported patch version %u\n", (unsigned)hdr->v);
    return TMD_STATUS_ERR_HDR;
  }

#if TMD_USE_CRC32
  if (hdr->algo != 1) {
    TMD_LOG("TinyMLDelta: algo=%u not supported (expected CRC32=1)\n", (unsigned)hdr->algo);
    return TMD_STATUS_ERR_UNSUPPORTED;
  }
#else
  (void)hdr;
#endif

  size_t off = sizeof(tmd_hdr_t);
  if (off + hdr->meta_len > patch_len) {
    return TMD_STATUS_ERR_HDR;
  }

  /* Parse metadata TLVs */
  tmd_meta_state_t meta;
  tmd_status_t st = tmd_parse_meta(patch + off, hdr->meta_len, &meta);
  if (st != TMD_STATUS_OK) return st;

  /* Guardrail checks */
  st = tmd_check_guardrails(&meta);
  if (st != TMD_STATUS_OK) return st;

  off += hdr->meta_len;

  uint8_t active = P->get_active_slot();
  uint8_t inactive = (active == 0) ? 1 : 0;
  const tmd_slot_t* slot_src = (active == 0) ? &L->slotA : &L->slotB;
  const tmd_slot_t* slot_dst = (inactive == 0) ? &L->slotA : &L->slotB;

  if (slot_src->size != slot_dst->size) {
    TMD_LOG("TinyMLDelta: slot size mismatch\n");
    return TMD_STATUS_ERR_PARAM;
  }

  /* Copy active slot to inactive slot */
  st = tmd_copy_slot(P, slot_src, slot_dst);
  if (st != TMD_STATUS_OK) return st;

#if TMD_FEAT_JOURNAL
  tmd_journal_t j = {0};
  if (!P->journal_read(&j) || j.magic != TMD_JOURNAL_MAGIC) {
    memset(&j, 0, sizeof(j));
    j.magic = TMD_JOURNAL_MAGIC;
    j.patch_id = 0; /* local-only; could be derived from header digest */
    j.next_chunk_idx = 0;
    j.target_slot = inactive;
  }
#else
  uint32_t next_chunk_idx = 0;
#endif

  uint8_t scratch[TMD_SCRATCH_SZ];

  /* Apply chunks */
  for (uint32_t idx = 0; idx < hdr->chunks_n; ++idx) {
    if (off + sizeof(tmd_chunk_hdr_t) > patch_len) {
      return TMD_STATUS_ERR_HDR;
    }
    const tmd_chunk_hdr_t* ch = (const tmd_chunk_hdr_t*)(patch + off);
    off += sizeof(tmd_chunk_hdr_t);

    uint32_t crc_val = 0;
#if TMD_FEAT_CRC32
    if (ch->has_crc) {
      if (off + 4 > patch_len) return TMD_STATUS_ERR_HDR;
      crc_val = (uint32_t)patch[off] |
                ((uint32_t)patch[off+1] << 8) |
                ((uint32_t)patch[off+2] << 16) |
                ((uint32_t)patch[off+3] << 24);
      off += 4;
    }
#endif
    if (off + ch->len > patch_len) {
      return TMD_STATUS_ERR_HDR;
    }
    const uint8_t* enc_data = patch + off;
    off += ch->len;

#if TMD_FEAT_CRC32
    if (ch->has_crc && P->crc32) {
      uint32_t got = P->crc32(enc_data, ch->len);
      if (got != crc_val) {
        TMD_LOG("TinyMLDelta: chunk CRC mismatch at idx=%lu\n",
                (unsigned long)idx);
        return TMD_STATUS_ERR_INTEGRITY;
      }
    }
#endif

    const uint8_t* data = NULL;
    uint32_t data_len = 0;

    if (ch->enc == 0) { /* RAW */
      data = enc_data;
      data_len = ch->len;
    } else if (ch->enc == 1) { /* RLE */
      if (tmd_rle_decode(enc_data, ch->len, scratch, sizeof(scratch), &data_len) != 0) {
        return TMD_STATUS_ERR_HDR;
      }
      data = scratch;
    } else {
      TMD_LOG("TinyMLDelta: unsupported encoding %u\n", (unsigned)ch->enc);
      return TMD_STATUS_ERR_UNSUPPORTED;
    }

    if (ch->off + data_len > slot_dst->size) {
      TMD_LOG("TinyMLDelta: chunk out of range (off=%lu,len=%lu,size=%lu)\n",
              (unsigned long)ch->off, (unsigned long)data_len,
              (unsigned long)slot_dst->size);
      return TMD_STATUS_ERR_PARAM;
    }

    uint32_t addr = slot_dst->addr + ch->off;
    if (!P->flash_write(addr, data, data_len)) {
      return TMD_STATUS_ERR_FLASH;
    }

#if TMD_FEAT_JOURNAL
    j.next_chunk_idx = idx + 1;
    P->journal_write(&j);
#endif
  }

#if TMD_FEAT_JOURNAL
  P->journal_clear();
#endif

  if (!P->set_active_slot(inactive)) {
    return TMD_STATUS_ERR_FLASH;
  }

  TMD_LOG("TinyMLDelta: patch applied OK, active slot=%u\n", (unsigned)inactive);
  return TMD_STATUS_OK;
}
