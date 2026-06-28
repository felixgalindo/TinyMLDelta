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

## Sample results

These are **deterministic** (seeded, no training) and reproducible with the
commands shown — a snapshot lives in [`sample_results.csv`](sample_results.csv).

### Compression backend vs. update type (`backends.py`)

The best backend depends on the *kind* of change — and bsdiff, while smallest, is
**not MCU-deployable** (O(n) RAM). On a 60 KB blob:

| Update payload | raw | rle | lz4 | bsdiff | gzip(full) |
|----------------|-----|-----|-----|--------|------------|
| sparse weight diff (~2%, high-entropy) | 5.95% | 5.95% | 5.98% | 5.40%¹ | 100.06% |
| repetitive / quant-like (low-entropy)  | 50.0% | **0.39%** | **0.21%** | 34.40%¹ | 50.35% |

¹ bsdiff = size reference only, **not on-device** (O(n) RAM). raw/rle/lz4 are deployable.

→ RLE/LZ4 dominate on repetitive (quant-churn) changes; on high-entropy weight
diffs no codec helps much. *Backend winner is a function of the update.*

### Same logical update across model formats (`formats.py`)

TinyMLDelta is format-agnostic; byte-diff behaviour depends on the container:

| Format | Update | Target | `.tmd` | Ratio |
|--------|--------|--------|--------|-------|
| flat (GGUF-like) | weight | 740 B | 96 B | 13.0% |
| flat | grow | 1008 B | 373 B | 37.0% |
| ONNX (protobuf) | weight | 864 B | 96 B | 11.1% |
| ONNX | grow | 1137 B | 427 B | 37.6% |

(Tiny demo models → overhead-dominated; larger models amortize it.)

```bash
.tinyenv/bin/python bench/formats.py                       # cross-format table
.tinyenv/bin/python bench/run_bench.py --quant int8        # model scenarios -> CSV
```

> Full model-scenario numbers (S1–S4 across trained models) require a real
> training run; `bench/results/` is git-ignored (generated). The tables above
> need no training and reproduce exactly.

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
| `backends.py` | Compression-backend comparison (RAW/RLE/LZ4/bsdiff/gzip) + on-device decode feasibility per backend |
| `formats.py` | Cross-format comparison (flat / ONNX / TFLite): same logical update, patch size per serialization format |

Optional extras for `backends.py` / `formats.py`:

```bash
.tinyenv/bin/python -m pip install lz4 bsdiff4 onnx   # enable LZ4/bsdiff/ONNX
```
