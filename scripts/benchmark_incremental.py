"""Controlled Windows AST-cache measurements; Python standard library only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import statistics

from accept_real_projects import run, save


def evidence(database):
    with sqlite3.connect(database) as con:
        con.row_factory = sqlite3.Row
        scan = con.execute("SELECT max(id) FROM scan").fetchone()[0]
        tables = ("file", "translation_unit", "unit_coverage", "symbol", "function_metric", "graph_edge", "coverage", "issue", "suppressed_issue", "rule_diagnostic")
        data = {table: sorted(json.dumps({k: row[k] for k in row.keys() if k != "scan_id"}, sort_keys=True)
                              for row in con.execute(f"SELECT * FROM {table} WHERE scan_id=?", (scan,))) for table in tables}
        analysis = dict(con.execute("SELECT * FROM analysis WHERE scan_id=?", (scan,)).fetchone())
        data["analysis"] = {k: v for k, v in analysis.items() if k not in ("scan_id", "elapsed_ms")}
        return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--units", type=int, default=64)
    parser.add_argument("--functions", type=int, default=128)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    if os.name != "nt" or not (8 <= args.units <= 512 and 16 <= args.functions <= 512 and 1 <= args.repeats <= 10 and 1 <= args.threads <= 64):
        parser.error("Windows and bounded units/functions/repeats/threads required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / "source"
    source.mkdir()
    (source / "common.h").write_text("#pragma once\ninline int adjust(int n){return n>0?n:0;}\n", encoding="utf-8")
    commands = []
    for i in range(args.units):
        name = f"unit{i:04}.cpp"
        content = '#include "common.h"\n' + "\n".join(
            f"int unit_{i}_function_{j}(int n){{int a[4]={{}};if(n>0&&n<4){{for(int k=0;k<n;++k)a[k]=k;}}return a[0]+adjust(n);}}" for j in range(args.functions))
        (source / name).write_text(content, encoding="utf-8")
        commands.append({"directory": source.as_posix(), "file": name, "arguments": ["clang++", "-std=c++20", "-c", name]})
    save(output / "compile_commands.json", commands)
    cli = args.cli.resolve()
    env = dict(os.environ)
    env["PATH"] = str(args.toolchain.resolve() / "mingw64/bin") + os.pathsep + env.get("PATH", "")
    rows = []
    for repeat in range(1, args.repeats + 1):
        database = output / f"repeat-{repeat}.sqlite3"
        baseline = None
        for mode in ("full", "cold", "warm"):
            log = output / f"repeat-{repeat}-{mode}.log"
            measurement = run([cli, "scan", source, "--database", database, "--compile-commands", output,
                               "--threads", args.threads, "--cache", "off" if mode == "full" else "on"], output, log, env)
            text = log.read_text(encoding="utf-8")
            metrics = {key: int(value) for key, value in re.findall(r"(?m)^(analysis_ms|function_metrics|cache_hits|cache_misses|cache_errors)=(\d+)", text)}
            actual = evidence(database)
            if baseline is None:
                baseline = actual
            if actual != baseline or metrics["function_metrics"] != args.units * args.functions + 1:
                raise RuntimeError("Full/cold/warm analysis evidence differs")
            if mode == "warm" and (metrics.get("cache_hits") != args.units or metrics.get("cache_errors") != 0):
                raise RuntimeError("Warm cache did not reuse all TU results")
            rows.append({"repeat": repeat, "mode": mode, **metrics, **measurement, "evidence_sha256": actual})
    changed = source / "unit0000.cpp"
    changed.write_text(changed.read_text(encoding="utf-8").replace("a[0]+adjust(n)", "a[1]+adjust(n)"), encoding="utf-8")
    incremental_hash = None
    for mode in ("one_changed", "changed_full"):
        log = output / f"{mode}.log"
        measurement = run([cli, "scan", source, "--database", database, "--compile-commands", output,
                           "--threads", args.threads, "--cache", "on" if mode == "one_changed" else "off"], output, log, env)
        metrics = {key: int(value) for key, value in re.findall(r"(?m)^(analysis_ms|cache_hits|cache_misses|cache_errors)=(\d+)", log.read_text(encoding="utf-8"))}
        actual = evidence(database)
        if mode == "one_changed":
            incremental_hash = actual
            if metrics.get("cache_hits") != args.units - 1 or metrics.get("cache_misses") != 1:
                raise RuntimeError("Single-file change did not invalidate exactly one TU")
        elif actual != incremental_hash:
            raise RuntimeError("Incremental analysis differs from a fresh full parse")
        rows.append({"mode": mode, **metrics, **measurement, "evidence_sha256": actual})
    means = {mode: statistics.mean(r["analysis_ms"] for r in rows if r["mode"] == mode) for mode in ("full", "cold", "warm")}
    summary = {"units": args.units, "functions_per_unit": args.functions, "threads": args.threads, "repeats": args.repeats,
               "mean_analysis_ms": means, "full_over_warm_speedup": means["full"] / means["warm"], "measurements": rows,
               "cli_sha256": hashlib.sha256(cli.read_bytes()).hexdigest(), "equivalence_passed": True,
               "limits": "Synthetic local source. OS file caches are not flushed; fixed full/cold/warm order. Peak memory sampled every 20 ms. Fresh preprocessing remains required on every hit; cold caching runs additional validation."}
    save(output / "summary.json", summary)
    print(json.dumps({k: v for k, v in summary.items() if k != "measurements"}, indent=2))


if __name__ == "__main__":
    main()
