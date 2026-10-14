/**
 * @file tmd_port_stm32l4.c
 * @brief TinyMLDelta platform port for the STM32L476 (SensorTile, STM32L476JG).
 *
 * The TinyMLDelta core treats a slot like RAM: it writes chunks at arbitrary
 * offsets, with arbitrary lengths, over bytes it has already written. NOR flash
 * cannot do that. On the STM32L4 specifically:
 *
 *   - the smallest erasable unit is a 2 KiB page;
 *   - the smallest programmable unit is a 64-bit doubleword at an 8-byte
 *     aligned address;
 *   - programming a doubleword that is not erased raises PGSERR, and on a part
 *     with flash ECC a second write to the same doubleword can latch an
 *     uncorrectable ECC fault on later reads.
 *
 * So this port sits a single-page write-back cache between the core and the
 * hardware:
 *
 *   flash_write() -> load page into RAM (if not cached) -> patch bytes in RAM
 *                    -> mark dirty. Nothing touches flash yet.
 *   page switch   -> flush: erase the page, program 2 KiB of doublewords.
 *   flash_read()  -> served from the cache when it overlaps the dirty page,
 *                    otherwise a direct memory read.
 *
 * That makes writes coherent: the core can read back exactly what it wrote.
 *
 * The journal is append-only: the core calls journal_write() once per chunk,
 * and erasing the journal page that often would be slow and wear the flash.
 * Records are appended until the page fills, and only then is it erased.
 *
 * Flash wear: journal_write() saves the cached page first, so the journal
 * never records a chunk before the chunk is in flash. The cost is one extra
 * erase of a slot page per chunk, e.g. about 34 erases for a 34-chunk patch,
 * and each page can be erased about 10,000 times. Build with
 * TMD_FEAT_JOURNAL=0 if the extra erases cost too much for your product.
 *
 * Address check: every erase and write must land inside the INACTIVE slot
 * (see region_ok()). The core never needs to write anywhere else, so a faulty
 * patch can't change the running model or the application.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#include <string.h>
#include "stm32l4xx_hal.h"
#include "tinymldelta_ports.h"
#include "flash_layout.h"
#include "board.h"
#include "tmd_port_stm32l4.h"

#define ERASED_DW  0xFFFFFFFFFFFFFFFFull   /* a doubleword of erased flash */
#define HAL_NO_ERR 0xFFFFFFFFu             /* HAL_FLASHEx_Erase: "no failed page" */

/* ==========================================================================
 * Reading flash directly
 * ==========================================================================
 * Flash is memory-mapped, so it is read through a pointer. This is the only
 * place in the firmware that turns a flash address into a pointer, and main.c
 * uses it too. */
const uint8_t *tmd_port_flash_ptr(uint32_t addr)
{
  return (const uint8_t *)addr;
}

static const uint8_t *flash_ptr(uint32_t addr)
{
  return tmd_port_flash_ptr(addr);
}

/* Little-endian loads and stores, byte by byte, so no alignment is assumed
 * and no object is read through a pointer of another type. */
static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
  return (uint64_t)get_le32(p) | ((uint64_t)get_le32(&p[4]) << 32);
}

static void put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* ==========================================================================
 * Page write-back cache
 * ========================================================================== */

static uint8_t  s_page[L4_PAGE_SZ];
static uint32_t s_page_addr;          /* page-aligned flash address, or 0 = none */
static bool     s_page_dirty;

static uint32_t page_base(uint32_t addr)
{
  return addr & ~(L4_PAGE_SZ - 1u);
}

/* ==========================================================================
 * Address fence
 * ==========================================================================
 * True if [addr, addr+len) lies entirely inside [base, base+size).
 * Written so that addr+len can never wrap around 32 bits. */
static bool in_range(uint32_t addr, uint32_t len, uint32_t base, uint32_t size)
{
  bool ok = false;

  if ((addr >= base) && ((addr - base) <= size)) {
    ok = (len <= (size - (addr - base)));
  }
  return ok;
}

static uint8_t port_get_active_slot(void);

/* The core only ever needs to change the INACTIVE slot: it copies the running
 * model there, patches it, and flips the marker last. So that is the only
 * place it may erase or write. The running model and the application are out
 * of reach, whatever the patch says. The application may also erase the
 * staging region once a patch has been consumed. */
static bool region_ok(uint32_t addr, uint32_t len, bool allow_staging)
{
  uint32_t inactive = (port_get_active_slot() == 0u) ? TMD_SLOT_B_ADDR : TMD_SLOT_A_ADDR;
  bool ok = in_range(addr, len, inactive, TMD_SLOT_BYTES);

  if ((!ok) && allow_staging) {
    ok = in_range(addr, len, TMD_STAGING_ADDR, TMD_STAGING_BYTES);
  }
  return ok;
}

/* Convert an absolute flash address to the (bank, page) pair HAL_FLASHEx_Erase
 * wants. The 1 MiB L476 runs dual-bank, so bank 2 restarts page numbering. */
static void addr_to_bank_page(uint32_t addr, uint32_t *bank, uint32_t *page)
{
  if (addr < L4_BANK2_BASE) {
    *bank = FLASH_BANK_1;
    *page = (addr - L4_FLASH_BASE) / L4_PAGE_SZ;
  } else {
    *bank = FLASH_BANK_2;
    *page = (addr - L4_BANK2_BASE) / L4_PAGE_SZ;
  }
}

static bool erase_page(uint32_t addr)
{
  FLASH_EraseInitTypeDef e;
  uint32_t err = HAL_NO_ERR;
  uint32_t bank = 0u;
  uint32_t page = 0u;
  bool ok = false;

  addr_to_bank_page(addr, &bank, &page);

  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Banks     = bank;
  e.Page      = page;
  e.NbPages   = 1u;

  if (HAL_FLASH_Unlock() == HAL_OK) {
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    ok = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK) && (err == HAL_NO_ERR);
    (void)HAL_FLASH_Lock();
  }
  return ok;
}

/* Program one doubleword. The target must be erased. */
static bool program_dw(uint32_t addr, uint64_t dw)
{
  bool ok = false;

  if (HAL_FLASH_Unlock() == HAL_OK) {
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    ok = (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dw) == HAL_OK);
    (void)HAL_FLASH_Lock();
  }
  return ok;
}

/* Program a whole 2 KiB page from RAM. The page must already be erased.
 * Doublewords that are still 0xFF..FF are skipped: they are already correct,
 * and skipping them saves a good fraction of the programming time. */
static bool program_page(uint32_t addr, const uint8_t *src)
{
  bool ok = true;

  for (uint32_t off = 0u; ok && (off < L4_PAGE_SZ); off += 8u) {
    uint64_t dw = get_le64(&src[off]);
    if (dw != ERASED_DW) {
      ok = program_dw(addr + off, dw);
    }
  }
  return ok;
}

static bool cache_flush(void)
{
  bool ok = true;

  if ((s_page_addr != 0u) && s_page_dirty) {
    ok = erase_page(s_page_addr) && program_page(s_page_addr, s_page);
    if (ok) {
      s_page_dirty = false;
    }
  }
  return ok;
}

static bool cache_load(uint32_t base)
{
  bool ok = true;

  if (s_page_addr != base) {
    ok = cache_flush();
    if (ok) {
      (void)memcpy(s_page, flash_ptr(base), L4_PAGE_SZ);
      s_page_addr  = base;
      s_page_dirty = false;
    }
  }
  return ok;
}

/* Force any pending page out to flash. The core has no "sync" hook, so the
 * application calls this after tmd_apply_patch_from_memory() returns. */
bool tmd_port_sync(void)
{
  return cache_flush();
}

/* ==========================================================================
 * Flash primitives exposed to the core
 * ==========================================================================
 * The void pointers in these signatures are fixed by tmd_ports_t; each one is
 * converted to a byte pointer once, at the top. */

static bool port_flash_read(uint32_t addr, void *dst, uint32_t len)
{
  uint8_t *out = (uint8_t *)dst;
  uint32_t a = addr;
  uint32_t done = 0u;

  /* reads may come from either slot (copy source and destination) */
  bool ok = in_range(addr, len, TMD_SLOT_A_ADDR, 2u * TMD_SLOT_BYTES);

  /* Serve any part of the range that overlaps the dirty cached page from RAM,
   * so the core always reads back what it just wrote. */
  while (ok && (done < len)) {
    uint32_t base = page_base(a);
    uint32_t off  = a - base;
    uint32_t n    = L4_PAGE_SZ - off;
    if (n > (len - done)) {
      n = len - done;
    }

    if (s_page_dirty && (base == s_page_addr)) {
      (void)memcpy(&out[done], &s_page[off], n);
    } else {
      (void)memcpy(&out[done], flash_ptr(a), n);
    }
    a    += n;
    done += n;
  }
  return ok;
}

static bool port_flash_write(uint32_t addr, const void *src, uint32_t len)
{
  const uint8_t *in = (const uint8_t *)src;
  uint32_t a = addr;
  uint32_t done = 0u;
  bool ok = region_ok(addr, len, false);

  if (!ok) {
    board_log("port: write refused @0x%08lx len=%lu (outside the inactive slot)\r\n",
              (unsigned long)addr, (unsigned long)len);
  }

  while (ok && (done < len)) {
    uint32_t base = page_base(a);
    uint32_t off  = a - base;
    uint32_t n    = L4_PAGE_SZ - off;
    if (n > (len - done)) {
      n = len - done;
    }

    ok = cache_load(base);
    if (ok) {
      (void)memcpy(&s_page[off], &in[done], n);
      s_page_dirty = true;
      a    += n;
      done += n;
    }
  }
  return ok;
}

static bool port_flash_erase(uint32_t addr, uint32_t len)
{
  bool ok = region_ok(addr, len, true);

  if (!ok) {
    board_log("port: erase refused @0x%08lx len=%lu (outside the inactive slot)\r\n",
              (unsigned long)addr, (unsigned long)len);
  } else {
    uint32_t a   = page_base(addr);
    uint32_t end = addr + len;      /* cannot wrap: region_ok() checked it */

    /* Drop the cache if it falls inside the region about to be erased, so a
     * stale dirty page cannot be flushed back over freshly erased flash. */
    if ((s_page_addr >= a) && (s_page_addr < end)) {
      s_page_addr  = 0u;
      s_page_dirty = false;
    } else {
      ok = cache_flush();
    }

    while (ok && (a < end)) {
      ok = erase_page(a);
      a += L4_PAGE_SZ;
    }
  }
  return ok;
}

/* ==========================================================================
 * Integrity
 * ========================================================================== */

/* Plain CRC-32 (reflected, poly 0xEDB88320) — the same one PatchGen uses.
 * The STM32's hardware CRC unit defaults to the unreflected CRC-32/MPEG-2
 * polynomial arrangement, so a software loop is used to guarantee the device
 * and the host agree byte for byte. main.c uses it too, via tmd_ports(). */
static uint32_t crc32_sw(const void *buf, size_t len)
{
  const uint8_t *p = (const uint8_t *)buf;
  uint32_t c = 0xFFFFFFFFu;

  for (size_t i = 0u; i < len; i++) {
    c ^= (uint32_t)p[i];
    for (uint32_t k = 0u; k < 8u; k++) {
      uint32_t mask = ((c & 1u) != 0u) ? 0xEDB88320u : 0u;
      c = (c >> 1) ^ mask;
    }
  }
  return c ^ 0xFFFFFFFFu;
}

/* ==========================================================================
 * Active-slot marker
 * ==========================================================================
 * One doubleword in its own page: [ 'T''M''D''A' | slot ].
 * Erased flash reads 0xFF..FF, which fails the magic test and means "slot A".
 */
#define MARKER_MAGIC  0x41444D54u   /* 'TMDA' little-endian */

static uint8_t port_get_active_slot(void)
{
  const uint8_t *m = flash_ptr(TMD_MARKER_ADDR);
  uint8_t slot = 0u;

  if (get_le32(m) == MARKER_MAGIC) {
    slot = (uint8_t)(get_le32(&m[4]) & 1u);
  }
  return slot;
}

static bool port_set_active_slot(uint8_t idx)
{
  /* low word = magic, high word = slot (little-endian doubleword) */
  uint64_t dw = ((uint64_t)idx & 1u);
  dw = (dw << 32) | (uint64_t)MARKER_MAGIC;

  /* new slot fully in flash first, then the marker */
  return cache_flush() && erase_page(TMD_MARKER_ADDR) && program_dw(TMD_MARKER_ADDR, dw);
}

/* ==========================================================================
 * Journal — append-only records
 * ==========================================================================
 * Record = 16 bytes (two doublewords):
 *   [0..3]   magic
 *   [4..7]   patch_id
 *   [8..11]  next_chunk_idx
 *   [12]     target_slot
 *   [13..15] pad (0xFF)
 *
 * The newest valid record wins. 2048/16 = 128 records fit before the page must
 * be erased. The core clears the journal after every successful patch, so the
 * page only fills during a patch with more than 128 chunks.
 */
#define JRNL_REC_SZ   16u
#define JRNL_RECS     (TMD_JOURNAL_BYTES / JRNL_REC_SZ)
#define JRNL_NONE     JRNL_RECS        /* "no record" index */

/* True if all n bytes at p are still erased (0xFF). */
static bool is_erased(const uint8_t *p, uint32_t n)
{
  bool erased = true;

  for (uint32_t i = 0u; erased && (i < n); i++) {
    erased = (p[i] == 0xFFu);
  }
  return erased;
}

/* Number of record slots in use. The core's journal magic value is private to
 * tinymldelta_core.c, so the port does not test for it; the core validates the
 * magic after journal_read().
 *
 * A record slot counts as used if ANY of its 16 bytes was programmed. Looking
 * only at the first word would treat a half-written record as free, and the
 * next append would then program a doubleword that is not erased. */
static uint32_t journal_used(void)
{
  const uint8_t *p = flash_ptr(TMD_JOURNAL_ADDR);
  uint32_t used = 0u;

  while ((used < JRNL_RECS) && (!is_erased(&p[used * JRNL_REC_SZ], JRNL_REC_SZ))) {
    used++;
  }
  return used;
}

static bool port_journal_read(tmd_journal_t *out)
{
  uint32_t used = journal_used();
  uint32_t newest = (used == 0u) ? JRNL_NONE : (used - 1u);

  (void)memset(out, 0, sizeof *out);

  if (newest != JRNL_NONE) {
    const uint8_t *p = flash_ptr(TMD_JOURNAL_ADDR + (newest * JRNL_REC_SZ));

    /* Power lost between the two doublewords of the newest record: it is
     * torn, so report "no journal" rather than hand the core half a record. */
    if ((!is_erased(p, 8u)) && (!is_erased(&p[8], 8u))) {
      out->magic          = get_le32(p);
      out->patch_id       = get_le32(&p[4]);
      out->next_chunk_idx = get_le32(&p[8]);
      out->target_slot    = p[12];
    }
  }
  return true;
}

static bool port_journal_write(const tmd_journal_t *in)
{
  uint8_t rec[JRNL_REC_SZ];
  uint32_t slot_i = journal_used();
  bool ok = cache_flush();

  if (ok && (slot_i >= JRNL_RECS)) {      /* page full — recycle it */
    ok = erase_page(TMD_JOURNAL_ADDR);
    slot_i = 0u;
  }

  if (ok) {
    uint32_t addr = TMD_JOURNAL_ADDR + (slot_i * JRNL_REC_SZ);

    (void)memset(rec, 0xFF, sizeof rec);
    put_le32(rec,     in->magic);
    put_le32(&rec[4], in->patch_id);
    put_le32(&rec[8], in->next_chunk_idx);
    rec[12] = in->target_slot;

    for (uint32_t o = 0u; ok && (o < JRNL_REC_SZ); o += 8u) {
      ok = program_dw(addr + o, get_le64(&rec[o]));
    }
  }
  return ok;
}

static bool port_journal_clear(void)
{
  return cache_flush() && erase_page(TMD_JOURNAL_ADDR);
}

/* ==========================================================================
 * Port table
 * ========================================================================== */

const tmd_ports_t *tmd_ports(void)
{
  static const tmd_ports_t ports = {
    .flash_erase     = port_flash_erase,
    .flash_write     = port_flash_write,
    .flash_read      = port_flash_read,
#if TMD_FEAT_CRC32
    .crc32           = crc32_sw,
#endif
    .get_active_slot = port_get_active_slot,
    .set_active_slot = port_set_active_slot,
#if TMD_FEAT_JOURNAL
    .journal_read    = port_journal_read,
    .journal_write   = port_journal_write,
    .journal_clear   = port_journal_clear,
#endif
#if TMD_FEAT_LOG
    .log             = board_log,
#endif
  };
  return &ports;
}

/* The slot map the core works with. Addresses come from flash_layout.h. */
const tmd_layout_t *tmd_layout(void)
{
  static const tmd_layout_t layout = {
    .slotA     = { .addr = TMD_SLOT_A_ADDR, .size = TMD_SLOT_BYTES },
    .slotB     = { .addr = TMD_SLOT_B_ADDR, .size = TMD_SLOT_BYTES },
    .meta_addr = TMD_JOURNAL_ADDR,
    .meta_size = TMD_JOURNAL_BYTES,
  };
  return &layout;
}
