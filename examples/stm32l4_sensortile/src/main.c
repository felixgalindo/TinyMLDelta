/**
 * @file main.c
 * @brief TinyMLDelta on the SensorTile — patch a model in real STM32 flash.
 *
 * What this firmware does, once, on every boot:
 *
 *   1. Report the flash map, the active slot, and a CRC32 of both model slots.
 *   2. Look in the staging region for a patch the host has flashed there.
 *   3. If one is present and intact, hand it to the real TinyMLDelta core, which
 *      copies the active slot to the inactive one, applies the diff chunks,
 *      CRC-verifies, and flips the active-slot marker.
 *   4. Report the result and re-CRC both slots.
 *   5. Erase the staging region so the next reset is a clean no-op.
 *   6. Blink a status code forever.
 *
 * Nothing here is simulated. The slots are real pages of the STM32L476's flash,
 * the core is the unmodified runtime/src/tinymldelta_core.c from the repo, and
 * the host can verify the outcome independently by reading the slot back over
 * SWD and diffing it against the expected model.
 *
 * LED codes (LED1, PG12):
 *   slow 1 Hz            idle — no patch was staged
 *   fast 5 Hz            a patch was applied successfully
 *   double-blink + pause a patch was staged but rejected or failed
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#include "stm32l4xx_hal.h"
#include "stm32l4xx_it.h"
#include "tinymldelta.h"
#include "tinymldelta_ports.h"
#include "flash_layout.h"
#include "board.h"
#include "tmd_port_stm32l4.h"

#if !TMD_FEAT_CRC32
#error "this example checks the staged patch with the port's CRC32: build with TMD_FEAT_CRC32=1"
#endif

/* Written by the host ahead of the patch bytes so the firmware knows how long
 * the patch is and can reject a partially-written staging region:
 *   [0..3] magic  [4..7] patch length  [8..11] patch CRC32  [12..15] reserved */
#define STAGE_MAGIC   0x53444D54u   /* 'TMDS' */
#define STAGE_HDR_SZ  16u

/* What happened this boot; also picks the LED pattern. */
typedef enum {
  RUN_IDLE = 0,       /* no patch was staged */
  RUN_APPLIED,        /* patch applied */
  RUN_FAILED          /* patch staged but rejected or failed */
} run_result_t;

/* ------------------------------------------------------------------------- */

/* A little-endian 32-bit word in flash. */
static uint32_t read_u32(uint32_t addr)
{
  const uint8_t *p = tmd_port_flash_ptr(addr);
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC over the model actually stored in a slot. The model length is not written
 * anywhere on the device, so CRC the whole slot: the trailing erased 0xFF bytes
 * are identical between slots and still give a stable, comparable fingerprint
 * that the host can reproduce from a slot readback. */
static uint32_t crc_slot(uint32_t addr)
{
  return tmd_ports()->crc32(tmd_port_flash_ptr(addr), TMD_SLOT_BYTES);
}

static const char *status_name(tmd_status_t s)
{
  const char *name;

  switch (s) {
    case TMD_STATUS_OK:              name = "OK";              break;
    case TMD_STATUS_ERR_PARAM:       name = "ERR_PARAM";       break;
    case TMD_STATUS_ERR_HDR:         name = "ERR_HDR";         break;
    case TMD_STATUS_ERR_INTEGRITY:   name = "ERR_INTEGRITY";   break;
    case TMD_STATUS_ERR_GUARDRAIL:   name = "ERR_GUARDRAIL";   break;
    case TMD_STATUS_ERR_FLASH:       name = "ERR_FLASH";       break;
    case TMD_STATUS_ERR_UNSUPPORTED: name = "ERR_UNSUPPORTED"; break;
    case TMD_STATUS_ERR_SIGNATURE:   name = "ERR_SIGNATURE";   break;
    case TMD_STATUS_ERR_INTERNAL:    name = "ERR_INTERNAL";    break;
    default:                         name = "ERR_UNKNOWN";     break;
  }
  return name;
}

static void report_slots(const char *when)
{
  g_status.crc_slot_a = crc_slot(TMD_SLOT_A_ADDR);
  g_status.crc_slot_b = crc_slot(TMD_SLOT_B_ADDR);

  board_log("active slot %s : %c\r\n", when, (tmd_ports()->get_active_slot() != 0u) ? 'B' : 'A');
  board_log("crc32 slotA = 0x%08lx\r\n", (unsigned long)g_status.crc_slot_a);
  board_log("crc32 slotB = 0x%08lx\r\n", (unsigned long)g_status.crc_slot_b);
}

/* Look for a staged patch and apply it. */
static run_result_t apply_staged_patch(void)
{
  const uint32_t max_len = TMD_STAGING_BYTES - STAGE_HDR_SZ;
  uint32_t magic     = read_u32(TMD_STAGING_ADDR);
  uint32_t patch_len = read_u32(TMD_STAGING_ADDR + 4u);
  uint32_t want_crc  = read_u32(TMD_STAGING_ADDR + 8u);
  const uint8_t *patch = tmd_port_flash_ptr(TMD_STAGING_ADDR + STAGE_HDR_SZ);
  run_result_t result = RUN_FAILED;

  if (magic != STAGE_MAGIC) {
    board_log("staging: empty (magic=0x%08lx) — nothing to do\r\n", (unsigned long)magic);
    result = RUN_IDLE;
  } else if ((patch_len == 0u) || (patch_len > max_len)) {
    board_log("staging: bad length %lu — refusing\r\n", (unsigned long)patch_len);
  } else {
    uint32_t crc = tmd_ports()->crc32(patch, patch_len);
    g_status.patch_len = patch_len;
    g_status.patch_crc = crc;

    board_log("staging: patch %lu bytes, crc32=0x%08lx (expected 0x%08lx)\r\n",
              (unsigned long)patch_len, (unsigned long)crc, (unsigned long)want_crc);

    if (crc != want_crc) {
      board_log("staging: CRC MISMATCH — refusing to apply\r\n");
    } else {
      board_log("--- applying patch ---\r\n");

      tmd_status_t st = tmd_apply_patch_from_memory(patch, patch_len);
      board_log_tail();   /* the result lines below always fit in the log */

      /* The core has no sync hook; push the last cached page out before we
       * judge the result or read anything back. */
      if (!tmd_port_sync()) {
        board_log("port: final flush FAILED\r\n");
        if (st == TMD_STATUS_OK) {
          st = TMD_STATUS_ERR_FLASH;
        }
      }

      g_status.apply_status = (int32_t)st;
      board_log("--- result: %s (%d) ---\r\n", status_name(st), (int)st);
      result = (st == TMD_STATUS_OK) ? RUN_APPLIED : RUN_FAILED;
    }
  }
  return result;
}

static void SystemClock_Config(void)
{
  /* Deliberately left on the reset default: MSI at 4 MHz. No PLL, no external
   * crystal, nothing to misconfigure. Flash programming timing on the L4 is
   * driven by an internal oscillator and is independent of the core clock, and
   * this example has no real-time work to do. */
}

static void blink(uint32_t on_ms, uint32_t off_ms)
{
  board_led(true);
  HAL_Delay(on_ms);
  board_led(false);
  HAL_Delay(off_ms);
}

int main(void)
{
  (void)HAL_Init();
  SystemClock_Config();
  board_led_init();
  board_log_init();

  if (g_status.magic != BOARD_STATUS_MAGIC) {
    g_status.magic = BOARD_STATUS_MAGIC;
    g_status.boot_count = 0u;
  }
  g_status.boot_count++;
  g_status.apply_status = -1;
  g_status.patch_len = 0u;
  g_status.patch_crc = 0u;

  board_log("\r\n=== TinyMLDelta — SensorTile (STM32L476JG) ===\r\n");
  board_log("boot #%lu  SystemCoreClock=%lu Hz\r\n",
            (unsigned long)g_status.boot_count, (unsigned long)SystemCoreClock);
  const unsigned long slot_bytes = TMD_SLOT_BYTES;
  board_log("slotA   0x%08lx  %lu bytes\r\n", (unsigned long)TMD_SLOT_A_ADDR, slot_bytes);
  board_log("slotB   0x%08lx  %lu bytes\r\n", (unsigned long)TMD_SLOT_B_ADDR, slot_bytes);
  board_log("journal 0x%08lx  marker 0x%08lx  staging 0x%08lx\r\n",
            (unsigned long)TMD_JOURNAL_ADDR, (unsigned long)TMD_MARKER_ADDR,
            (unsigned long)TMD_STAGING_ADDR);

  g_status.slot_active_before = tmd_ports()->get_active_slot();
  report_slots("BEFORE");

  run_result_t result = apply_staged_patch();

  g_status.slot_active_after = tmd_ports()->get_active_slot();
  report_slots("AFTER ");

  /* Consume the patch so a reset does not replay it. */
  if (result != RUN_IDLE) {
    if (tmd_ports()->flash_erase(TMD_STAGING_ADDR, TMD_STAGING_BYTES)) {
      board_log("staging: erased\r\n");
    } else {
      board_log("staging: erase FAILED\r\n");
    }
  }

  board_log("=== done ===\r\n");

  for (;;) {
    if (result == RUN_APPLIED) {          /* fast: success */
      blink(100u, 100u);
    } else if (result == RUN_FAILED) {    /* double blink: failure */
      blink(80u, 80u);
      blink(80u, 760u);
    } else {                              /* slow: idle */
      blink(500u, 500u);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Exception handlers, called by the hardware through the vector table in ST's
 * startup file (declared in stm32l4xx_it.h). */

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void HardFault_Handler(void)
{
  /* Fast panic flicker, so a fault is visibly different from every normal code.
   * Busy loops, because SysTick may not be running in a fault. */
  board_led_init();
  for (;;) {
    board_led(true);
    for (volatile uint32_t i = 0u; i < 20000u; i++) { }
    board_led(false);
    for (volatile uint32_t i = 0u; i < 20000u; i++) { }
  }
}

void NMI_Handler(void)        { for (;;) { } }
void MemManage_Handler(void)  { HardFault_Handler(); }
void BusFault_Handler(void)   { HardFault_Handler(); }
void UsageFault_Handler(void) { HardFault_Handler(); }
void SVC_Handler(void)        { }
void DebugMon_Handler(void)   { }
void PendSV_Handler(void)     { }
