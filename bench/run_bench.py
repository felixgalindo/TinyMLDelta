#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_bench.py — TinyMLDelta real-world update benchmark (ResNet-8 / CIFAR-10).

Trains a real MLPerf Tiny model on a real dataset, then drives it through the
realistic field-update scenarios in scenarios.py, measuring the TinyMLDelta
patch size for each (float32 and int8) against gzip / bsdiff / detools baselines.
Results are written to a CSV summarizing patch sizes across scenarios.

Quick smoke test (a few minutes, small subset):
    python3 run_bench.py --quick

Full run (stable numbers; train longer):
    python3 run_bench.py --epochs-base 60 --quant both

Author:  Felix Galindo
License: Apache-2.0
"""

import argparse
import csv
import os
import sys
import time

import numpy as np

import data
import scenarios
from convert import convert, to_tflite_int8
from patchsize import measure
from resnet8 import build_resnet8

_HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(_HERE, "results")


def _acc(model, x, y) -> float:
    logits = model.predict(x, verbose=0, batch_size=256)
    return float((logits.argmax(1) == y).mean())


def train_base(xtr, ytr, xte, yte, epochs: int, lr: float, verbose: int):
    print(f"[base] training ResNet-8 for {epochs} epochs ...")
    m = build_resnet8()
    m.compile(
        optimizer=__import__("tensorflow").keras.optimizers.Adam(lr),
        loss=__import__("tensorflow").keras.losses.SparseCategoricalCrossentropy(from_logits=True),
        metrics=["accuracy"],
    )
    t0 = time.time()
    m.fit(xtr, ytr, epochs=epochs, batch_size=128, verbose=verbose,
          validation_data=(xte, yte))
    print(f"[base] clean test acc = {_acc(m, xte, yte):.3f}  "
          f"({time.time() - t0:.0f}s)")
    return m


def patch_pair(base_model, target_model, rep_clean, rep_drift, quant_modes,
               scenario, detail, xte, yte, rows):
    """Convert base+target under each quant mode, measure patch, append rows."""
    for q in quant_modes:
        # S4 quant recalibration is the special case: same weights, the *base*
        # is calibrated on clean data and the *target* on drifted data.
        rep_for_base = rep_clean
        rep_for_target = rep_drift if scenario == "S4_quant_recal" else rep_clean
        if q == "int8":
            base_b = to_tflite_int8(base_model, rep_for_base)
            tgt_b = to_tflite_int8(target_model, rep_for_target)
        else:
            base_b = convert(base_model, q, rep_clean)
            tgt_b = convert(target_model, q, rep_clean)

        r = measure(base_b, tgt_b)
        r.update({
            "scenario": scenario,
            "detail": detail,
            "quant": q,
            "target_acc": round(_acc(target_model, xte, yte), 4),
        })
        rows.append(r)
        ratio = f"{r['tmd_ratio']*100:.1f}%" if r["tmd_ratio"] else "n/a"
        print(f"  [{scenario:16s} {q:7s} {detail:14s}] "
              f"target={r['target_bytes']:>7d}B  tmd={r['tmd_bytes']:>6d}B "
              f"({ratio})  chunks={r['tmd_chunks']}  "
              f"gzip_full={r['gzip_full_bytes']}B")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--epochs-base", type=int, default=40)
    ap.add_argument("--quant", choices=["float32", "int8", "both"], default="both")
    ap.add_argument("--drift-severity", type=int, default=3, help="1..5 (CIFAR-10-C)")
    ap.add_argument("--sweep", default="1,2,5,10,20",
                    help="S1 fine-tune epoch sweep (comma list)")
    ap.add_argument("--out", default=os.path.join(RESULTS_DIR, "bench_results.csv"))
    ap.add_argument("--quick", action="store_true",
                    help="tiny subset + few epochs to validate the pipeline fast")
    ap.add_argument("--verbose", type=int, default=0)
    args = ap.parse_args()

    quant_modes = ["float32", "int8"] if args.quant == "both" else [args.quant]
    os.makedirs(RESULTS_DIR, exist_ok=True)

    # ---- Data -------------------------------------------------------------- #
    (xtr, ytr), (xte, yte) = data.load_cifar10()
    if args.quick:
        xtr, ytr = xtr[:5000], ytr[:5000]
        xte, yte = xte[:2000], yte[:2000]
        args.epochs_base = min(args.epochs_base, 3)
        sweep = [1, 2]
    else:
        sweep = [int(s) for s in args.sweep.split(",")]

    # Real-world drift: the field distribution the deployed model now sees.
    xtr_drift = data.make_drift(xtr, severity=args.drift_severity, seed=1)
    xte_drift = data.make_drift(xte, severity=args.drift_severity, seed=2)

    rep_clean = xtr
    rep_drift = xtr_drift

    # ---- Base model -------------------------------------------------------- #
    base = train_base(xtr, ytr, xte, yte, args.epochs_base, 1e-3, args.verbose)
    print(f"[drift] base acc on clean={_acc(base, xte, yte):.3f}  "
          f"on drifted={_acc(base, xte_drift, yte):.3f}  "
          f"(severity {args.drift_severity}) — drift motivates the update\n")

    rows = []

    # ---- S2 domain shift recovery (headline field update) ------------------ #
    print("== S2 domain_shift: recover accuracy after real field drift ==")
    tgt = scenarios.domain_shift(base, xtr_drift, ytr, epochs=10,
                                 verbose=args.verbose)
    print(f"   recovered drifted acc = {_acc(tgt, xte_drift, yte):.3f}")
    patch_pair(base, tgt, rep_clean, rep_drift, quant_modes,
               "S2_domain_shift", "recover", xte_drift, yte, rows)

    # ---- S3 head-only update (on-device personalization) ------------------- #
    print("== S3 head_update: retrain only the classifier head ==")
    tgt = scenarios.head_update(base, xtr_drift, ytr, epochs=10,
                                verbose=args.verbose)
    patch_pair(base, tgt, rep_clean, rep_drift, quant_modes,
               "S3_head_update", "head_only", xte_drift, yte, rows)

    # ---- S1 incremental fine-tune sweep (divergence vs patch size) --------- #
    print("== S1 incremental_finetune: patch size vs. fine-tune epochs ==")
    for ep in sweep:
        tgt = scenarios.incremental_finetune(base, xtr_drift, ytr, epochs=ep,
                                             verbose=args.verbose)
        patch_pair(base, tgt, rep_clean, rep_drift, quant_modes,
                   "S1_incremental", f"ep{ep}", xte_drift, yte, rows)

    # ---- S4 quantization recalibration (int8 only) ------------------------- #
    if "int8" in quant_modes:
        print("== S4 quant_recalibration: same weights, recalibrate int8 PTQ ==")
        # target model == base weights; only the calibration data differs.
        patch_pair(base, base, rep_clean, rep_drift, ["int8"],
                   "S4_quant_recal", "recal", xte, yte, rows)

    # ---- Write CSV --------------------------------------------------------- #
    fields = ["scenario", "detail", "quant", "base_bytes", "target_bytes",
              "tmd_bytes", "tmd_chunks", "tmd_ratio", "gzip_full_bytes",
              "bsdiff_bytes", "detools_bytes", "target_acc"]
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) for k in fields})
    print(f"\nWrote {len(rows)} rows -> {args.out}")


if __name__ == "__main__":
    main()
