/**
 * @file string.h
 * @brief Minimal freestanding <string.h>.
 *
 * This build links no libc, so the real newlib header is not present. Both the
 * ST HAL and the unmodified TinyMLDelta core include <string.h>; this supplies
 * exactly the declarations they use: memcpy and memset. The definitions live in board.c.
 *
 * -Iinc is first on the include path, so this is what `#include <string.h>`
 * resolves to.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#ifndef TMD_FREESTANDING_STRING_H_
#define TMD_FREESTANDING_STRING_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* TMD_FREESTANDING_STRING_H_ */
