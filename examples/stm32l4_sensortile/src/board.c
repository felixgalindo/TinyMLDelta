/**
 * @file board.c
 * @brief SensorTile board services + the freestanding libc shims this build needs.
 *
 * The Homebrew arm-none-eabi-gcc is a bare compiler with no newlib, so anything
 * the HAL or the TinyMLDelta core would normally get from libc is provided here.
 * That is not a workaround so much as the usual embedded arrangement: it keeps
 * the image small and makes every byte of the binary accounted for.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#include <stdarg.h>
#include <string.h>
#include "stm32l4xx_hal.h"
#include "board.h"

/* ==========================================================================
 * Freestanding libc: the two functions the HAL, the core and GCC's own
 * struct copies need. Their names and signatures are fixed by the C standard.
 * ========================================================================== */

void *memcpy(void *d, const void *s, size_t n)
{
  uint8_t *dp = (uint8_t *)d;
  const uint8_t *sp = (const uint8_t *)s;

  for (size_t i = 0u; i < n; i++) {
    dp[i] = sp[i];
  }
  return d;
}

void *memset(void *d, int c, size_t n)
{
  uint8_t *dp = (uint8_t *)d;
  uint8_t v = (uint8_t)c;

  for (size_t i = 0u; i < n; i++) {
    dp[i] = v;
  }
  return d;
}

/* ==========================================================================
 * LED — PG12. GPIOG sits in the VDDIO2 supply domain, which is off out of
 * reset; without EnableVddIO2 the pin simply never drives.
 * ========================================================================== */

void board_led_init(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWREx_EnableVddIO2();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  g.Pin   = GPIO_PIN_12;
  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &g);
}

void board_led(bool on)
{
  HAL_GPIO_WritePin(GPIOG, GPIO_PIN_12, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ==========================================================================
 * Log sink
 * ========================================================================== */

#define LOG_CAP  (16u * 1024u)   /* the core logs one line per copy chunk and two per
                                  * diff chunk; 8K was not enough headroom */

/* All three are volatile on purpose. Nothing in the firmware ever reads the
 * log back — the host does, over SWD, after the run. Without volatile the
 * compiler sees only dead stores and deletes the whole buffer; the first build
 * that turned semihosting off did exactly that, and s_log disappeared from the
 * image entirely. volatile is what tells GCC these writes are observable. */
__attribute__((section(".noinit"), used)) volatile board_status_t g_status;
__attribute__((section(".noinit"), used)) static volatile char     s_log[LOG_CAP];
__attribute__((section(".noinit"), used)) static volatile uint32_t s_log_len;

/* Semihosting is OFF by default, and that is a deliberate, hard-won default.
 *
 * The obvious guard — "only trap if DHCSR.C_DEBUGEN says a debugger is
 * attached" — does not work. C_DEBUGEN is set by the probe the first time it
 * enables debug and stays latched until a power cycle, so it still reads 1 long
 * after st-flash has disconnected. The firmware then executes BKPT #0xAB with
 * nothing listening, and the core halts on that instruction forever. That is
 * exactly what happened on the first run of this firmware: the log stopped dead
 * after the banner, DFSR read 0x2 (BKPT) and every fault register was clear.
 *
 * There is no reliable runtime test for "someone is listening for semihosting
 * calls", so it is a build-time choice. The RAM log below is the primary
 * channel and needs no debugger at all; build with -DBOARD_SEMIHOSTING=1 and
 * run under `make monitor` (OpenOCD with `arm semihosting enable`) when live
 * output is wanted.
 */
#ifndef BOARD_SEMIHOSTING
#define BOARD_SEMIHOSTING 0
#endif

#if BOARD_SEMIHOSTING
static void semihost_write0(const char *s)
{
  register void *r0 __asm__("r0") = (void *)0x04;   /* SYS_WRITE0 */
  register void *r1 __asm__("r1") = (void *)s;
  __asm__ volatile ("bkpt #0xAB" : "+r"(r0) : "r"(r1) : "memory");
}
#endif

/* The last LOG_TAIL bytes are kept free for the result lines main() prints
 * after the apply. A big patch (200 chunks) fills the log, and without this the
 * final "=== done ===" line, which the host waits for, would be lost. */
#define LOG_TAIL  512u

static bool s_log_tail_open;    /* set by board_log_tail() */
static bool s_log_full_noted;

static void log_putc(char c)
{
  static const char note[] = "\r\n[log full: lines dropped]\r\n";
  uint32_t n = s_log_len;
  uint32_t limit = s_log_tail_open ? (LOG_CAP - 1u) : (LOG_CAP - 1u - LOG_TAIL);

  if (n < limit) {
    s_log[n] = c;
    s_log_len = n + 1u;
  } else if ((!s_log_tail_open) && (!s_log_full_noted)) {
    uint32_t k = (uint32_t)(sizeof note) - 1u;
    s_log_full_noted = true;
    for (uint32_t i = 0u; i < k; i++) {
      s_log[n + i] = note[i];
    }
    s_log_len = n + k;
  } else {
    /* log full: drop the character */
  }
}

void board_log_tail(void)
{
  s_log_tail_open = true;
}

static void log_puts(const char *s)
{
  for (uint32_t i = 0u; s[i] != '\0'; i++) {
    log_putc(s[i]);
  }
}

/* Unsigned number in base 10 or 16, right-aligned in `width` characters. */
static void log_num(unsigned long v, unsigned long base, bool upper, uint32_t width, bool zero)
{
  static const char lo[] = "0123456789abcdef";
  static const char up[] = "0123456789ABCDEF";
  const char *dig = upper ? up : lo;
  char tmp[24];
  uint32_t n = 0u;
  unsigned long x = v;

  do {
    tmp[n] = dig[x % base];
    n++;
    x /= base;
  } while (x != 0u);

  for (uint32_t i = n; i < width; i++) {
    log_putc(zero ? '0' : ' ');
  }
  while (n > 0u) {
    n--;
    log_putc(tmp[n]);
  }
}

/* One conversion: `conv` is the letter after any flags, width and length. */
static void log_conv(char conv, va_list *ap, bool lng, uint32_t width, bool zero)
{
  switch (conv) {
    case 'd':
    case 'i': {
      long v = lng ? va_arg(*ap, long) : (long)va_arg(*ap, int);
      unsigned long mag = (unsigned long)v;
      if (v < 0) {
        log_putc('-');
        mag = 0UL - mag;            /* magnitude without signed overflow */
      }
      log_num(mag, 10u, false, width, zero);
      break;
    }
    case 'u':
      log_num(lng ? va_arg(*ap, unsigned long) : (unsigned long)va_arg(*ap, unsigned int),
              10u, false, width, zero);
      break;
    case 'x':
    case 'X':
      log_num(lng ? va_arg(*ap, unsigned long) : (unsigned long)va_arg(*ap, unsigned int),
              16u, (conv == 'X'), width, zero);
      break;
    case 'p': {
      const void *ptr = va_arg(*ap, const void *);
      log_puts("0x");
      log_num((unsigned long)(uintptr_t)ptr, 16u, false, 8u, true);
      break;
    }
    case 'c':
      log_putc((char)va_arg(*ap, int));
      break;
    case 's': {
      const char *str = va_arg(*ap, const char *);
      log_puts((str != NULL) ? str : "(null)");
      break;
    }
    case '%':
      log_putc('%');
      break;
    default:
      log_putc('%');
      log_putc(conv);
      break;
  }
}

/* printf-style logger. Supports %s %c %d %i %u %x %X %p %%, the 'l' and 'z'
 * length modifiers, zero padding and a width ("%08lx"). That covers every
 * format string the TinyMLDelta core uses. The core's log hook is variadic,
 * so this function has to be too. */
void board_log(const char *fmt, ...)
{
  va_list ap;
  uint32_t start = s_log_len;
  uint32_t i = 0u;

  va_start(ap, fmt);
  while (fmt[i] != '\0') {
    if (fmt[i] != '%') {
      log_putc(fmt[i]);
      i++;
    } else {
      bool zero = false;
      bool lng = false;
      uint32_t width = 0u;

      i++;
      if (fmt[i] == '0') {
        zero = true;
        i++;
      }
      while ((fmt[i] >= '0') && (fmt[i] <= '9')) {
        width = (width * 10u) + ((uint32_t)(uint8_t)fmt[i] - 48u);   /* 48 = '0' */
        i++;
      }
      while ((fmt[i] == 'l') || (fmt[i] == 'z')) {
        lng = true;
        i++;
      }
      if (fmt[i] != '\0') {
        log_conv(fmt[i], &ap, lng, width, zero);
        i++;
      }
    }
  }
  va_end(ap);

  s_log[s_log_len] = '\0';
  g_status.log_len = s_log_len;

#if BOARD_SEMIHOSTING
  semihost_write0((const char *)&s_log[start]);
#else
  (void)start;
#endif
}

void board_log_init(void)
{
  s_log_tail_open  = false;
  s_log_full_noted = false;
  s_log_len = 0u;
  s_log[0]  = '\0';
  g_status.log_len = 0u;
}

/* ==========================================================================
 * C runtime init
 * ==========================================================================
 * ST's startup file calls __libc_init_array, which normally comes from newlib
 * and runs C++ static constructors. This firmware is plain C and has none: the
 * linker script stops the build if .init_array is ever non-empty. So this
 * stand-in does nothing, and the startup file stays unmodified.
 */
void __libc_init_array(void)
{
}
