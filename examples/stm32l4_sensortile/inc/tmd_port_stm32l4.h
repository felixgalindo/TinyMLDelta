/**
 * @file tmd_port_stm32l4.h
 * @brief What the STM32L4 port offers the application, beyond tmd_ports().
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#ifndef TMD_PORT_STM32L4_H_
#define TMD_PORT_STM32L4_H_

#include <stdbool.h>
#include <stdint.h>

/* Write the cached flash page out, if it has changes. Call it after
 * tmd_apply_patch_from_memory() returns: the core has no "sync" hook. */
bool tmd_port_sync(void);

/* Pointer to a byte of memory-mapped flash, for reading it directly. */
const uint8_t *tmd_port_flash_ptr(uint32_t addr);

#endif /* TMD_PORT_STM32L4_H_ */
