# TinyMLDelta Benchmark Harness

Reproducible benchmark of **TinyMLDelta patch sizes for real-world model
updates**, using a recognized TinyML model (MLPerf Tiny **ResNet-8**) on a real
dataset (**CIFAR-10**), driven through the field-update scenarios TinyMLDelta is
designed for. It produces CSV (and optional LaTeX/Markdown) reports summarizing
patch sizes across update scenarios and model representations.

## What it measures

For each update scenario, in both **float32** and **int8** (deployment-realistic)
representations, it reports the TinyMLDelta `.tmd` patch size and ratio vs. the
full model, alongside baselines (gzip of the full model; bsdiff/detools if
installed).

### Update scenarios (the things TinyMLDelta is for)

| ID | Scenario | Real-world meaning |
|----|----------|--------------------|
| **S2** | Domain-shift recovery | Camera deployed into dim/foggy/noisy conditions; full-network fine-tune to recover accuracy. The headline field update. |
| **S3** | Head-only update | On-device personalization / transfer learning — freeze the backbone, retrain only the classifier head. Best case for delta patching. |
| **S1** | Incremental fine-tune sweep | Periodic retrain on new field data for N epochs — patch size vs. model divergence. |
| **S4** | Quantization recalibration | Identical weights, re-run int8 PTQ on drifted calibration data — isolates how much delta is pure quantization churn. |

Field drift is generated with CIFAR-10-C–style corruptions (brightness, fog,
sensor noise) — the actual distribution shifts that *create* the need to update.

## Run

```bash
# from repo root, using the project venv
source .tinyenv/bin/activate            # or: PY=.tinyenv/bin/python

# fast smoke test (subset + few epochs) — validates the pipeline in minutes
.tinyenv/bin/python bench/run_bench.py --quick

# full run for stable numbers (train the base model properly)
.tinyenv/bin/python bench/run_bench.py --epochs-base 60 --quant both

# emit LaTeX (booktabs) tables from the results
.tinyenv/bin/python bench/to_latex.py bench/results/bench_results.csv
```

Results are written to `bench/results/bench_results.csv`.

### Useful flags
- `--quant {float32,int8,both}` — representation(s) to test (default `both`).
- `--drift-severity 1..5` — CIFAR-10-C corruption strength (default 3).
- `--sweep 1,2,5,10,20` — epoch counts for the S1 divergence sweep.
- `--quick` — tiny subset + ≤3 epochs to validate end-to-end fast.

## Optional stronger baselines

The harness always reports gzip-of-full-model. For bsdiff / detools comparison:

```bash
.tinyenv/bin/python -m pip install bsdiff4 detools
```

They are picked up automatically if importable; otherwise those columns show `---`.

## Key finding to expect

For **int8** deployed models the patch ratio is dominated by **quantization
parameter churn**, not weight change: a full fine-tune + re-quantize can rewrite
most of the model, while a **head-only update** stays compact. This directly
quantifies the quantization-sensitivity effect and motivates update strategies
(head-only / quant-aware fine-tuning) that keep deltas small.

## Files

| File | Purpose |
|------|---------|
| `resnet8.py` | MLPerf Tiny ResNet-8 reference architecture |
| `data.py` | CIFAR-10 loader + realistic drift corruptions |
| `convert.py` | Keras → TFLite (float32 / int8 PTQ) |
| `scenarios.py` | S1–S3 update scenarios |
| `patchsize.py` | PatchGen wrapper + gzip/bsdiff/detools baselines |
| `run_bench.py` | Orchestrator → `results/bench_results.csv` |
| `to_latex.py` | CSV → LaTeX (booktabs) tables |
