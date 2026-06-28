/**
 * @file unit_tests.c
 * @brief Per-function unit tests for TinyMLDelta core internals.
 *
 * The core's helpers are `static`, so we #include the core .c directly to reach
 * them. Feature flags and firmware-guardrail constants are #defined first so the
 * gated code (LZ4, COPY/ADD) compiles and the guardrail thresholds are known.
 *
 * License: Apache-2.0
 */
#define TMD_FEAT_LZ4TINY        1
#define TMD_FEAT_COPYADD        1
#define TMD_FIRMWARE_OPSET_HASH 0x1234abcdu
#define TMD_ENFORCE_IO_HASH     1
#define TMD_FIRMWARE_IO_HASH    0xdeadbeefu

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../runtime/src/tinymldelta_core.c"

/* Linker stubs: the core references these (used only by apply()), which the
 * unit tests never call. */
const tmd_ports_t* tmd_ports(void) { return NULL; }
const tmd_layout_t* tmd_layout(void) { return NULL; }

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                  \
    if (cond) { g_pass++; }                                    \
    else { g_fail++; printf("  [FAIL] %s\n", (msg)); }         \
  } while (0)

/* ---- mock flash for tmd_rle_decode_write ------------------------------- */
static uint8_t g_flash[8192];
static bool mock_write(uint32_t addr, const void* src, uint32_t len) {
  if (addr + len > sizeof(g_flash)) return false;
  memcpy(g_flash + addr, src, len);
  return true;
}

/* ---- tests ------------------------------------------------------------- */

static void test_rle_decoded_len(void) {
  uint8_t a[] = {3, 'A', 2, 'B'};                 /* AAABB -> 5 */
  CHECK(tmd_rle_decoded_len(a, 4) == 5, "rle_decoded_len basic");
  uint8_t b[] = {0, 'X'};                          /* count 0 => 256 */
  CHECK(tmd_rle_decoded_len(b, 2) == 256, "rle_decoded_len run-of-256");
}

static void test_rle_decode_write(void) {
  tmd_ports_t P;
  memset(&P, 0, sizeof(P));
  P.flash_write = mock_write;
  uint8_t scratch[1024];
  uint8_t in[] = {4, 'A', 3, 'B'};                 /* AAAABBB */
  memset(g_flash, 0, sizeof(g_flash));
  tmd_status_t st = tmd_rle_decode_write(&P, in, 4, scratch, sizeof(scratch), 0);
  CHECK(st == TMD_STATUS_OK, "rle_decode_write status");
  CHECK(memcmp(g_flash, "AAAABBB", 7) == 0, "rle_decode_write output");
}

static void test_lz4_literals(void) {
  uint8_t dst[64];
  uint8_t blk[] = {0x50, 'H', 'e', 'l', 'l', 'o'}; /* token litlen=5, literals */
  int n = tmd_lz4_block_decode(blk, sizeof(blk), dst, sizeof(dst));
  CHECK(n == 5 && memcmp(dst, "Hello", 5) == 0, "lz4 literals-only");
}

static void test_lz4_match(void) {
  uint8_t dst[64];
  /* literals "AB", then match offset=2 len=4 -> "ABABAB" */
  uint8_t blk[] = {0x20, 'A', 'B', 0x02, 0x00};
  int n = tmd_lz4_block_decode(blk, sizeof(blk), dst, sizeof(dst));
  CHECK(n == 6 && memcmp(dst, "ABABAB", 6) == 0, "lz4 back-reference match");
}

static void test_lz4_overflow_rejected(void) {
  uint8_t dst[3];
  uint8_t blk[] = {0x50, 'H', 'e', 'l', 'l', 'o'}; /* needs 5, cap 3 */
  CHECK(tmd_lz4_block_decode(blk, sizeof(blk), dst, sizeof(dst)) < 0,
        "lz4 output-overflow rejected");
}

static void test_guardrails(void) {
  tmd_meta_state_t m;
  memset(&m, 0, sizeof(m));
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_OK, "guard empty accepts");

  memset(&m, 0, sizeof(m));
  m.req_arena_bytes = TMD_FIRMWARE_ARENA_BYTES + 1;
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_ERR_GUARDRAIL, "guard arena over");
  m.req_arena_bytes = TMD_FIRMWARE_ARENA_BYTES;
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_OK, "guard arena at-limit");

  memset(&m, 0, sizeof(m));
  m.tflm_abi = TMD_FIRMWARE_TFLM_ABI + 1;
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_ERR_GUARDRAIL, "guard abi over");

  memset(&m, 0, sizeof(m));
  m.opset_hash = 0xdeadbeefu;                      /* != firmware opset */
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_ERR_GUARDRAIL, "guard opset mismatch");
  m.opset_hash = 0x1234abcdu;                      /* == firmware opset */
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_OK, "guard opset match");

  memset(&m, 0, sizeof(m));
  m.io_hash = 0x11111111u;                         /* != firmware io (enforced) */
  CHECK(tmd_check_guardrails(&m) == TMD_STATUS_ERR_GUARDRAIL, "guard io mismatch");
}

int main(void) {
  test_rle_decoded_len();
  test_rle_decode_write();
  test_lz4_literals();
  test_lz4_match();
  test_lz4_overflow_rejected();
  test_guardrails();
  printf("\nC unit tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
