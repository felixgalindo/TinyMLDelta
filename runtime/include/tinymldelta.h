#ifndef TINYMLDELTA_H_
#define TINYMLDELTA_H_
/**
 * @file tinymldelta.h
 * @brief TinyMLDelta — Public C API for applying patches.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TMD_STATUS_OK = 0,
  TMD_STATUS_ERR_PARAM,
  TMD_STATUS_ERR_HDR,
  TMD_STATUS_ERR_INTEGRITY,
  TMD_STATUS_ERR_GUARDRAIL,
  TMD_STATUS_ERR_FLASH,
  TMD_STATUS_ERR_UNSUPPORTED,
  TMD_STATUS_ERR_INTERNAL
} tmd_status_t;

/**
 * @brief Apply a TinyMLDelta patch from memory to the inactive slot.
 *
 * Flow:
 *   - Parse header and TLVs from @p patch.
 *   - Validate metadata against firmware guardrails.
 *   - Copy active slot to inactive slot.
 *   - Apply diff chunks into inactive slot.
 *   - Optionally verify final digest.
 *   - Flip active slot.
 *
 * The exact flash layout and HAL details are provided by the application via
 * tmd_ports() and tmd_layout().
 *
 * @param patch     Pointer to patch bytes.
 * @param patch_len Length of patch buffer in bytes.
 * @return ::TMD_STATUS_OK on success, error code otherwise.
 */
tmd_status_t tmd_apply_patch_from_memory(const uint8_t* patch, size_t patch_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYMLDELTA_H_ */
