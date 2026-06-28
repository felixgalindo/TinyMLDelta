/**
 * @file envelope.c
 * @brief Capability-envelope check implementation. License: Apache-2.0
 */
#include "envelope.h"

tmd_env_result_t tmd_envelope_check(const tmd_envelope_t *env,
                                    const tmd_patch_caps_t *p,
                                    const char **why) {
  /* 1) Operators: the target's ops must be a SUBSET of the linked set.
   *    (used_ops & ~linked_ops) are ops the firmware didn't link → can't run. */
  uint32_t missing = p->used_ops & ~env->linked_ops;
  if (missing) {
    if (why) *why = "target uses an operator the firmware did not link";
    return ENV_REJECT_OPSET;
  }

  /* 2) Arena: required activation memory must fit the provisioned arena. */
  if (p->req_arena_bytes > env->arena_bytes) {
    if (why) *why = "target arena requirement exceeds provisioned arena";
    return ENV_REJECT_ARENA;
  }

  /* 3) Slot: a growing model must still fit the inactive slot. */
  if (p->target_len > env->slot_bytes) {
    if (why) *why = "target model larger than the flash slot";
    return ENV_REJECT_SLOT;
  }

  /* 4) I/O: every I/O axis this update changes must be one the app can absorb. */
  uint32_t unsupported = p->io_change & ~env->io_flex;
  if (unsupported) {
    if (why) *why = "target changes an I/O axis the app cannot absorb";
    return ENV_REJECT_IO;
  }

  if (why) *why = "within envelope";
  return ENV_ACCEPT;
}

const char *tmd_env_result_str(tmd_env_result_t r) {
  switch (r) {
    case ENV_ACCEPT:        return "ACCEPT";
    case ENV_REJECT_OPSET:  return "REJECT(opset)";
    case ENV_REJECT_ARENA:  return "REJECT(arena)";
    case ENV_REJECT_SLOT:   return "REJECT(slot)";
    case ENV_REJECT_IO:     return "REJECT(io)";
    default:                return "REJECT(?)";
  }
}
