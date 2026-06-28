/**
 * @file schema.c
 * @brief I/O schema validate + configure (app side). License: Apache-2.0
 */
#include "schema.h"

tmd_schema_result_t tmd_schema_apply(const tmd_io_schema_t *s,
                                     const tmd_tensor_contract_t *t,
                                     const tmd_app_caps_t *app,
                                     uint16_t *out_classes,
                                     float *out_threshold,
                                     const char **why) {
  /* 1) Version gate: refuse a schema newer than this app understands. */
  if (s->schema_version > app->supported_schema_max) {
    if (why) *why = "schema_version newer than the app supports";
    return SCHEMA_REJECT_VERSION;
  }

  /* 2) dtype the app actually has a code path for. */
  if (!(app->supported_dtypes & (1u << s->in_dtype))) {
    if (why) *why = "input dtype unsupported by the app";
    return SCHEMA_REJECT_DTYPE;
  }

  /* 3) Schema must agree with the live tensor contract (Layer 1). A mismatch
   *    means the schema and the model bytes disagree → corrupt/incompatible. */
  if (s->in_h != t->in_h || s->in_w != t->in_w || s->in_c != t->in_c ||
      s->in_dtype != t->in_dtype) {
    if (why) *why = "schema input shape/dtype disagrees with the model tensors";
    return SCHEMA_REJECT_TENSOR_MISMATCH;
  }
  if (s->task == TASK_CLASSIFY && s->n_labels != t->out_n) {
    if (why) *why = "schema label count disagrees with the output tensor";
    return SCHEMA_REJECT_TENSOR_MISMATCH;
  }

  /* 4) Can the sensor/feature pipeline physically supply this input? */
  if (s->in_h > app->pipeline_max_h || s->in_w > app->pipeline_max_w) {
    if (why) *why = "input larger than the feature pipeline can supply";
    return SCHEMA_REJECT_PIPELINE;
  }

  /* Configure: the app now reads class count / threshold from the contract. */
  if (out_classes)   *out_classes   = (s->task == TASK_CLASSIFY) ? t->out_n : 0;
  if (out_threshold) *out_threshold = s->threshold;
  if (why) *why = "schema accepted; app reconfigured";
  return SCHEMA_OK;
}

const char *tmd_schema_result_str(tmd_schema_result_t r) {
  switch (r) {
    case SCHEMA_OK:                    return "ACCEPT";
    case SCHEMA_REJECT_VERSION:        return "REJECT(version)";
    case SCHEMA_REJECT_TENSOR_MISMATCH:return "REJECT(tensor-mismatch)";
    case SCHEMA_REJECT_DTYPE:          return "REJECT(dtype)";
    case SCHEMA_REJECT_PIPELINE:       return "REJECT(pipeline)";
    default:                           return "REJECT(?)";
  }
}
