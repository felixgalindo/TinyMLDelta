#ifndef FLASH_LAYOUT_H_
#define FLASH_LAYOUT_H_

#include <stdint.h>
#include "tinymldelta_ports.h"

/* Simple layout for flash.bin:
 *   slotA: [0 .. 64KiB-1]
 *   slotB: [64KiB .. 128KiB-1]
 *   meta : [128KiB .. 132KiB-1]
 */

static const tmd_layout_t g_layout = {
    .slotA = { .addr = 0u,         .size = 64u * 1024u },
    .slotB = { .addr = 64u*1024u,  .size = 64u * 1024u },
    .meta_addr = 128u * 1024u,
    .meta_size = 4u * 1024u,
};

#endif /* FLASH_LAYOUT_H_ */
