/**
 * @file board.h
 * @brief SensorTile board services: LED, logging, status block.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#ifndef BOARD_H_
#define BOARD_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- LED (STEVAL-STLCS01V1: LED1 = PG12, active high, VDDIO2 domain) ------ */
void board_led_init(void);
void board_led(bool on);

/* --- logging -------------------------------------------------------------- */
/* printf-style. Appends to a RAM buffer the host reads over SWD after the run;
 * with BOARD_SEMIHOSTING=1 it also prints live through OpenOCD. Supports
 * %s %c %d %i %u %x %X %p %%, the 'l' and 'z' length modifiers, zero padding
 * and a width ("%08lx") — every format string the TinyMLDelta core uses. */
void board_log(const char *fmt, ...);
void board_log_init(void);
/* Open the reserved end of the log for the final result lines. */
void board_log_tail(void);

/* Called by ST's startup file; see board.c. */
void __libc_init_array(void);

/* --- status block --------------------------------------------------------- */
/* Lives at a fixed RAM address in .noinit so the host can read it back with
 * OpenOCD after the firmware has run. */
#define BOARD_STATUS_MAGIC   0x31534D54u   /* 'TMS1' */

typedef struct {
  uint32_t magic;
  uint32_t boot_count;
  uint32_t slot_active_before;
  uint32_t slot_active_after;
  uint32_t crc_slot_a;
  uint32_t crc_slot_b;
  uint32_t patch_len;        /* 0 = no patch staged this boot */
  uint32_t patch_crc;
  int32_t  apply_status;     /* tmd_status_t, -1 = not attempted */
  uint32_t log_len;
} board_status_t;

extern volatile board_status_t g_status;

#endif /* BOARD_H_ */
