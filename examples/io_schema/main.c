/**
 * @file main.c
 * @brief Reference demo + self-test for the TinyMLDelta I/O schema (app side).
 *
 * Each case simulates a model update arriving with a schema (carried in model
 * metadata or a vendor TLV) plus the live tensor contract the interpreter would
 * expose. The app re-syncs: version-check -> validate vs tensors -> confirm the
 * pipeline can supply the input -> accept (reconfigure) or reject (keep old slot).
 *
 * Doubles as an integration test: exits non-zero on any unexpected decision.
 * License: Apache-2.0
 */
#include <stdio.h>
#include "schema.h"

int main(void) {
  /* This firmware/app: understands schema <= v3, MFCC pipeline up to 49x13,
   * handles int8 + float32. */
  const tmd_app_caps_t app = {
      .supported_schema_max = 3,
      .pipeline_max_h = 49, .pipeline_max_w = 13,
      .supported_dtypes = (1u << DT_INT8) | (1u << DT_FLOAT32),
  };

  struct {
    const char *name;
    tmd_io_schema_t schema;
    tmd_tensor_contract_t tensors;
    tmd_schema_result_t expect;
  } cases[] = {
    /* 1) baseline KWS classifier, v2 — accepted */
    {"baseline v2",
     {2, 49,10,1, DT_INT8, PRE_MFCC, 0,0, 0, TASK_CLASSIFY, 4,
      {"silence","unknown","yes","no"}, 0.0f},
     {49,10,1, DT_INT8, 4}, SCHEMA_OK},

    /* 2) added a class (4 -> 5); app reads n_classes from the tensor — accepted */
    {"add class",
     {3, 49,10,1, DT_INT8, PRE_MFCC, 0,0, 0, TASK_CLASSIFY, 5,
      {"silence","unknown","yes","no","up"}, 0.0f},
     {49,10,1, DT_INT8, 5}, SCHEMA_OK},

    /* 3) anomaly model, changed normalization + threshold — accepted */
    {"renorm anomaly",
     {2, 1,4,1, DT_FLOAT32, PRE_ZSCORE, 25.1f,3.0f, 4, TASK_ANOMALY, 0,
      {0}, 0.04f},
     {1,4,1, DT_FLOAT32, 1}, SCHEMA_OK},

    /* 4) schema_version newer than the app understands — rejected */
    {"future schema",
     {7, 49,10,1, DT_INT8, PRE_MFCC, 0,0, 0, TASK_CLASSIFY, 4,
      {"silence","unknown","yes","no"}, 0.0f},
     {49,10,1, DT_INT8, 4}, SCHEMA_REJECT_VERSION},

    /* 5) schema says 5 labels but the output tensor has 4 — corrupt/incompatible */
    {"label mismatch",
     {3, 49,10,1, DT_INT8, PRE_MFCC, 0,0, 0, TASK_CLASSIFY, 5,
      {"silence","unknown","yes","no","up"}, 0.0f},
     {49,10,1, DT_INT8, 4}, SCHEMA_REJECT_TENSOR_MISMATCH},

    /* 6) model wants 64x64 input the MFCC pipeline can't supply — rejected */
    {"bigger input",
     {3, 64,64,1, DT_INT8, PRE_MFCC, 0,0, 0, TASK_CLASSIFY, 4,
      {"silence","unknown","yes","no"}, 0.0f},
     {64,64,1, DT_INT8, 4}, SCHEMA_REJECT_PIPELINE},
  };

  const int n = (int)(sizeof(cases) / sizeof(cases[0]));
  int failures = 0;

  printf("App: schema<=v%u, pipeline<=%ux%u, dtypes int8+float32\n\n",
         app.supported_schema_max, app.pipeline_max_h, app.pipeline_max_w);

  for (int i = 0; i < n; i++) {
    uint16_t classes = 0; float thr = 0.0f; const char *why = NULL;
    tmd_schema_result_t got =
        tmd_schema_apply(&cases[i].schema, &cases[i].tensors, &app,
                         &classes, &thr, &why);
    int ok = (got == cases[i].expect);
    failures += !ok;
    printf("  %-16s -> %-22s %-48s %s\n",
           cases[i].name, tmd_schema_result_str(got), why,
           ok ? "" : "[UNEXPECTED]");
    if (got == SCHEMA_OK)
      printf("       %22s configured: classes=%u threshold=%.3f\n",
             "", classes, thr);
  }

  printf("\n%s (%d/%d as expected)\n",
         failures ? "FAIL" : "OK", n - failures, n);
  return failures ? 1 : 0;
}
