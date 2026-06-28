/**
 * @file schema.h
 * @brief Reference implementation of the TinyMLDelta I/O schema (app side).
 *
 * When a model update can change I/O, the app must re-sync to the new contract.
 * The *tensor* part (shapes/dtype/quant) comes free from the interpreter; the
 * *semantic* part (labels, normalization, task, threshold) does not and rides
 * with the model — in the TFLite model metadata buffer or an opaque vendor TLV.
 * TinyMLDelta carries it opaquely and gates by version; the app owns the format.
 * See docs/io-schema-integration.md.
 *
 * This example is the application side: parse → version-check → validate against
 * the live tensors → confirm the pipeline can satisfy it → accept or reject.
 *
 * Self-contained: no TFLM needed; builds with any C99 cc.
 * License: Apache-2.0
 */
#ifndef TMD_SCHEMA_H_
#define TMD_SCHEMA_H_

#include <stdint.h>
#include <stdbool.h>

#define TMD_MAX_LABELS 16

typedef enum { DT_INT8 = 0, DT_FLOAT32 = 1 } tmd_dtype_t;
typedef enum { TASK_ANOMALY = 0, TASK_CLASSIFY = 1 } tmd_task_t;
typedef enum { PRE_NONE = 0, PRE_ZSCORE = 1, PRE_MFCC = 2 } tmd_pre_t;

/* The semantic + preprocessing schema carried with the model (Layer 2). */
typedef struct {
  uint16_t schema_version;
  /* input */
  uint16_t in_h, in_w, in_c;
  tmd_dtype_t in_dtype;
  tmd_pre_t pre;           /* preprocessing the app must apply */
  float mean, std;         /* PRE_ZSCORE params */
  uint16_t window;
  /* output */
  tmd_task_t task;
  uint16_t n_labels;       /* classification: number of labels (0 for anomaly) */
  const char *labels[TMD_MAX_LABELS];
  float threshold;         /* anomaly decision threshold */
} tmd_io_schema_t;

/* Layer 1: what the interpreter exposes at runtime for the loaded model. */
typedef struct {
  uint16_t in_h, in_w, in_c;
  tmd_dtype_t in_dtype;
  uint16_t out_n;          /* output last dim: n_classes, or 1 for anomaly */
} tmd_tensor_contract_t;

/* What THIS firmware/app can physically support. */
typedef struct {
  uint16_t supported_schema_max;       /* highest schema_version understood    */
  uint16_t pipeline_max_h, pipeline_max_w; /* what the feature pipeline emits   */
  uint32_t supported_dtypes;           /* bitmask: (1u << tmd_dtype_t)          */
} tmd_app_caps_t;

typedef enum {
  SCHEMA_OK = 0,
  SCHEMA_REJECT_VERSION,        /* schema_version newer than app understands */
  SCHEMA_REJECT_TENSOR_MISMATCH,/* schema disagrees with the live tensors    */
  SCHEMA_REJECT_DTYPE,          /* dtype the app has no code path for        */
  SCHEMA_REJECT_PIPELINE        /* input the sensor/pipeline cannot supply    */
} tmd_schema_result_t;

/* Validate + (conceptually) configure the app from a schema. Returns the first
 * failing reason or SCHEMA_OK. On OK, *out_classes / *out_threshold reflect the
 * runtime-read contract the app would now use. */
tmd_schema_result_t tmd_schema_apply(const tmd_io_schema_t *s,
                                     const tmd_tensor_contract_t *t,
                                     const tmd_app_caps_t *app,
                                     uint16_t *out_classes,
                                     float *out_threshold,
                                     const char **why);

const char *tmd_schema_result_str(tmd_schema_result_t r);

#endif /* TMD_SCHEMA_H_ */
