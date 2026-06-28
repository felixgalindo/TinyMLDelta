/**
 * @file main.c
 * @brief Reference demo + self-test for the TinyMLDelta capability envelope.
 *
 * A firmware build declares one envelope, then several candidate update patches
 * are checked against it. This shows the guardrails accepting in-envelope
 * architecture changes (added class, widened layer) while rejecting what truly
 * needs a firmware update (new operator, arena/slot overflow, unsupported I/O).
 *
 * Doubles as an integration test: exits non-zero if any decision is unexpected.
 *
 * License: Apache-2.0
 */
#include <stdio.h>
#include "envelope.h"

/* The op set a typical CNN/DS-CNN firmware links (no LSTM, no transpose-conv). */
#define LINKED_OPS (OP_CONV2D | OP_DWCONV2D | OP_FULLYCONN | OP_MAXPOOL | \
                    OP_AVGPOOL | OP_MEAN | OP_SOFTMAX | OP_RELU | OP_ADD | OP_RESHAPE)

int main(void) {
  /* Worked example from docs/capability-envelope.md (Cortex-M4 class). */
  const tmd_envelope_t env = {
      .linked_ops = LINKED_OPS,
      .arena_bytes = 48 * 1024,
      .slot_bytes  = 160 * 1024,
      .io_flex     = IO_FLEX_CLASS_COUNT | IO_FLEX_INPUT_RES | IO_FLEX_QUANT,
  };

  struct {
    tmd_patch_caps_t caps;
    tmd_env_result_t expect;
  } cases[] = {
    /* weight-only update — same arch, trivially in-envelope */
    {{"weight update",   LINKED_OPS, 36 * 1024, 96 * 1024,  IO_FLEX_NONE},        ENV_ACCEPT},
    /* add a class — bigger output; app reads n_classes at runtime */
    {{"add class",       LINKED_OPS, 37 * 1024, 96 * 1024,  IO_FLEX_CLASS_COUNT}, ENV_ACCEPT},
    /* widen a layer — same ops, more activations, still fits arena+slot */
    {{"widen layer",     LINKED_OPS, 45 * 1024, 150 * 1024, IO_FLEX_NONE},        ENV_ACCEPT},
    /* higher input resolution — app reads H/W; fits arena */
    {{"input 32->48",    LINKED_OPS, 47 * 1024, 96 * 1024,  IO_FLEX_INPUT_RES},   ENV_ACCEPT},
    /* NEW operator (LSTM) the firmware never linked — must reject */
    {{"add LSTM",        LINKED_OPS | OP_LSTM, 40 * 1024, 96 * 1024, IO_FLEX_NONE}, ENV_REJECT_OPSET},
    /* arena overflow — model needs more activation memory than provisioned */
    {{"too big arena",   LINKED_OPS, 64 * 1024, 96 * 1024,  IO_FLEX_NONE},        ENV_REJECT_ARENA},
    /* model larger than the slot can hold */
    {{"oversize model",  LINKED_OPS, 40 * 1024, 200 * 1024, IO_FLEX_NONE},        ENV_REJECT_SLOT},
    /* changes input resolution but app pipeline can't supply it (flag off here) */
    {{"unsupported io",  LINKED_OPS, 40 * 1024, 96 * 1024,
      IO_FLEX_CLASS_COUNT | (1u << 5) /* an axis not in env.io_flex */},          ENV_REJECT_IO},
  };

  const int n = (int)(sizeof(cases) / sizeof(cases[0]));
  int failures = 0;

  printf("Capability envelope: linked-ops=0x%03x arena=%uKB slot=%uKB\n\n",
         env.linked_ops, env.arena_bytes / 1024, env.slot_bytes / 1024);

  for (int i = 0; i < n; i++) {
    const char *why = NULL;
    tmd_env_result_t got = tmd_envelope_check(&env, &cases[i].caps, &why);
    int ok = (got == cases[i].expect);
    failures += !ok;
    printf("  %-16s -> %-14s  %-45s %s\n",
           cases[i].caps.name, tmd_env_result_str(got), why,
           ok ? "" : "[UNEXPECTED]");
  }

  printf("\n%s (%d/%d as expected)\n",
         failures ? "FAIL" : "OK", n - failures, n);
  return failures ? 1 : 0;
}
