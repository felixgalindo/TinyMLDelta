/**
 * @file envelope.h
 * @brief Reference implementation of the TinyMLDelta *capability envelope*.
 *
 * A firmware build declares an envelope: the set of operators it linked, the
 * arena it provisioned, the largest model slot it can hold, and which I/O
 * changes its app code can absorb. PatchGen (or the device) checks each patch's
 * declared requirements against this envelope and decides accept/reject —
 * reframing the guardrails from "reject any architecture change" to "accept
 * within the declared envelope, reject only on overflow."
 *
 * This mirrors the real guardrail TLVs (REQ_ARENA_BYTES, OPSET_HASH, IO_HASH,
 * target_len) — see docs/capability-envelope.md.
 *
 * Self-contained: no TFLM or TinyMLDelta core needed; builds with any C99 cc.
 *
 * License: Apache-2.0
 */
#ifndef TMD_ENVELOPE_H_
#define TMD_ENVELOPE_H_

#include <stdint.h>
#include <stdbool.h>

/* Operator kinds a model might use. In a real build this maps to the linked
 * MicroMutableOpResolver set; here we use a bitmask so "subset" is explicit. */
typedef enum {
  OP_CONV2D      = 1u << 0,
  OP_DWCONV2D    = 1u << 1,
  OP_FULLYCONN   = 1u << 2,
  OP_MAXPOOL     = 1u << 3,
  OP_AVGPOOL     = 1u << 4,
  OP_MEAN        = 1u << 5,
  OP_SOFTMAX     = 1u << 6,
  OP_RELU        = 1u << 7,
  OP_ADD         = 1u << 8,
  OP_RESHAPE     = 1u << 9,
  OP_LSTM        = 1u << 10,   /* a "new" op a CNN firmware likely didn't link */
  OP_TRANSPOSECONV = 1u << 11
} tmd_op_t;

/* I/O flexibility the app supports (it reads these dims/params at runtime). */
typedef enum {
  IO_FLEX_NONE        = 0,
  IO_FLEX_CLASS_COUNT = 1u << 0, /* app reads n_classes from the output tensor */
  IO_FLEX_INPUT_RES   = 1u << 1, /* app reads H/W from the input tensor       */
  IO_FLEX_QUANT       = 1u << 2  /* app reads scale/zero_point at runtime     */
} tmd_io_flex_t;

/* What the firmware build provisioned (its capability envelope). */
typedef struct {
  uint32_t linked_ops;       /* bitmask of tmd_op_t the resolver links     */
  uint32_t arena_bytes;      /* TMD_FIRMWARE_ARENA_BYTES                    */
  uint32_t slot_bytes;       /* max model size a slot can hold              */
  uint32_t io_flex;          /* bitmask of tmd_io_flex_t the app supports   */
} tmd_envelope_t;

/* What a candidate patch's target model requires (from its TLVs / metadata). */
typedef struct {
  const char *name;
  uint32_t used_ops;         /* bitmask of tmd_op_t the target model uses   */
  uint32_t req_arena_bytes;  /* REQ_ARENA_BYTES                             */
  uint32_t target_len;       /* target model size                          */
  uint32_t io_change;        /* bitmask of tmd_io_flex_t this update changes */
} tmd_patch_caps_t;

typedef enum {
  ENV_ACCEPT = 0,
  ENV_REJECT_OPSET,
  ENV_REJECT_ARENA,
  ENV_REJECT_SLOT,
  ENV_REJECT_IO
} tmd_env_result_t;

/* Check a patch against the envelope. Returns the first failing reason, or
 * ENV_ACCEPT. If `why` is non-NULL it receives a human-readable explanation. */
tmd_env_result_t tmd_envelope_check(const tmd_envelope_t *env,
                                    const tmd_patch_caps_t *p,
                                    const char **why);

const char *tmd_env_result_str(tmd_env_result_t r);

#endif /* TMD_ENVELOPE_H_ */
