#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
to_latex.py — Turn bench_results.csv into LaTeX (booktabs) tables.

    python3 to_latex.py results/bench_results.csv

Emits, to stdout, ready-to-include tables:
  - per-scenario patch ratio (float32 vs int8)
  - the S1 fine-tune divergence sweep
  - a baseline comparison (TinyMLDelta vs gzip / bsdiff / detools)

Author:  Felix Galindo
License: Apache-2.0
"""

import csv
import sys
from collections import defaultdict


def _f(v):
    return None if v in (None, "", "None") else float(v)


def load(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def pct(x):
    return f"{x*100:.1f}\\%" if x is not None else "---"


def table_scenarios(rows):
    by = {(r["scenario"], r["quant"]): r for r in rows
          if r["scenario"] != "S1_incremental"}
    scen_order = ["S2_domain_shift", "S3_head_update", "S4_quant_recal"]
    names = {
        "S2_domain_shift": "Domain shift (full retrain)",
        "S3_head_update": "Head-only update",
        "S4_quant_recal": "Quant recalibration",
    }
    out = [
        "\\begin{table}[h]\\centering",
        "\\caption{Patch size by real-world update scenario (ResNet-8, CIFAR-10)}",
        "\\label{tab:scenarios}\\small",
        "\\begin{tabular}{@{}llrrr@{}}\\toprule",
        "\\textbf{Scenario} & \\textbf{Repr.} & \\textbf{Target} & "
        "\\textbf{Patch} & \\textbf{Ratio} \\\\ \\midrule",
    ]
    for s in scen_order:
        for q in ("float32", "int8"):
            r = by.get((s, q))
            if not r:
                continue
            out.append(
                f"{names[s]} & {q} & {int(r['target_bytes']):,} B & "
                f"{int(r['tmd_bytes']):,} B & {pct(_f(r['tmd_ratio']))} \\\\")
    out += ["\\bottomrule\\end{tabular}\\end{table}"]
    return "\n".join(out)


def table_sweep(rows):
    sw = [r for r in rows if r["scenario"] == "S1_incremental"]
    by_q = defaultdict(list)
    for r in sw:
        by_q[r["quant"]].append(r)
    out = [
        "\\begin{table}[h]\\centering",
        "\\caption{Patch size vs.\\ fine-tuning epochs (ResNet-8, CIFAR-10)}",
        "\\label{tab:granularity}\\small",
        "\\begin{tabular}{@{}lrrr@{}}\\toprule",
        "\\textbf{Repr.} & \\textbf{Epochs} & \\textbf{Patch} & "
        "\\textbf{Ratio} \\\\ \\midrule",
    ]
    for q in ("float32", "int8"):
        for r in sorted(by_q.get(q, []), key=lambda x: int(x["detail"][2:])):
            ep = r["detail"][2:]
            out.append(f"{q} & {ep} & {int(r['tmd_bytes']):,} B & "
                       f"{pct(_f(r['tmd_ratio']))} \\\\")
    out += ["\\bottomrule\\end{tabular}\\end{table}"]
    return "\n".join(out)


def table_baselines(rows):
    # use int8 rows (deployment-realistic) where baselines exist
    out = [
        "\\begin{table}[h]\\centering",
        "\\caption{TinyMLDelta vs.\\ general delta/compression tools (int8)}",
        "\\label{tab:comparison}\\small",
        "\\begin{tabular}{@{}llrrrr@{}}\\toprule",
        "\\textbf{Scenario} & \\textbf{TinyMLDelta} & \\textbf{gzip(full)} & "
        "\\textbf{bsdiff} & \\textbf{detools} \\\\ \\midrule",
    ]
    for r in rows:
        if r["quant"] != "int8":
            continue
        def b(k):
            v = _f(r.get(k))
            return f"{int(v):,} B" if v is not None else "---"
        out.append(
            f"{r['scenario'].replace('_', ' ')} & {b('tmd_bytes')} & "
            f"{b('gzip_full_bytes')} & {b('bsdiff_bytes')} & {b('detools_bytes')} \\\\")
    out += ["\\bottomrule\\end{tabular}\\end{table}"]
    return "\n".join(out)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "results/bench_results.csv"
    rows = load(path)
    print(table_scenarios(rows))
    print("\n")
    print(table_sweep(rows))
    print("\n")
    print(table_baselines(rows))


if __name__ == "__main__":
    main()
