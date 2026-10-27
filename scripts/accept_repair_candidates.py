"""Reproduce the deterministic repair demo with real CLI processes (stdlib only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess

REPO = Path(__file__).resolve().parents[1]


def hashes(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--core-cli", type=Path)
    parser.add_argument("--batch", action="store_true", help="Also exercise batch ranking and Core batch limitations")
    parser.add_argument("--coverage", action="store_true", help="Collect gcov data and demonstrate an unexecuted target line")
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out, cli = args.output.resolve(), args.cli.resolve(strict=True)
    if out.is_relative_to(REPO / "samples/repair-demo"):
        raise ValueError("acceptance output must be outside the original sample")
    out.mkdir(parents=True, exist_ok=False)
    toolbin = args.toolchain.resolve(strict=True) / "mingw64/bin"
    env = os.environ.copy()
    env["PATH"] = str(toolbin) + os.pathsep + env["PATH"]
    root, db = out / "demo-source", out / "scan.sqlite3"
    sample = REPO / "samples/repair-demo"
    original = hashes(sample)
    shutil.copytree(sample, root)
    executions = []

    def run(label, command, exits=(0,)):
        command = [str(x) for x in command]
        print("RUN", label, flush=True)
        result = subprocess.run(command, cwd=out, env=env, capture_output=True, timeout=180)
        stdout = result.stdout.decode("utf-8", errors="replace")
        (out / (label + ".log")).write_bytes(result.stdout + b"\n--- stderr ---\n" + result.stderr)
        executions.append({"label": label, "command": command, "exit_code": result.returncode})
        save(out / "commands.json", executions)
        if result.returncode not in exits:
            raise RuntimeError(f"{label}: exit {result.returncode}; inspect log")
        return stdout

    configure = out / "configure"
    cc, cxx = toolbin / "gcc.exe", toolbin / "g++.exe"
    coverage_args = ["--coverage", "gcov", "--gcov-tool", toolbin / "gcov.exe"] if args.coverage else []
    run("configure", [toolbin / "cmake.exe", "-S", root, "-B", configure, "-G", "Ninja",
                      "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DCMAKE_CXX_COMPILER={cxx}"])
    run("scan", [cli, "scan", root, "--database", db, "--compile-commands", configure])
    with sqlite3.connect(db) as connection:
        scan_id = connection.execute("SELECT MAX(id) FROM scan").fetchone()[0]
        issues = list(connection.execute("SELECT rule_id,file,line,col FROM issue WHERE scan_id=?", (scan_id,)))
    build = [cli, "build", root, "--database", db, "--output", out / "original-runs",
             "--c-compiler", cc, "--cxx-compiler", cxx, "--jobs", "2", "--timeout", "60"]
    # The demo deliberately fails its original behavioral test.
    run("original-build", build, (2,))
    with sqlite3.connect(db) as connection:
        original_steps = list(connection.execute("SELECT name,status,exit_code FROM build_step WHERE run_id=(SELECT MAX(id) FROM build_run)"))
    assert any(s[0] == "build" and s[1] == "passed" for s in original_steps), original_steps
    assert any(s[0] == "test" and s[1] == "failed" for s in original_steps), original_steps
    candidates, results = {}, {}
    for rule in ("CG003", "CG001"):
        matches = [i for i in issues if i[0] == rule]
        assert len(matches) == 1, matches
        _, file, line, column = matches[0]
        text = run("propose-" + rule, [cli, "repair", "propose", root, "--database", db,
                   "--output", out / "candidates", "--rule", rule, "--file", file,
                   "--line", line, "--column", column])
        for row in text.splitlines():
            fields = row.split("\t")
            if len(fields) == 3:
                candidates[fields[1]] = Path(fields[2])
    assert set(candidates) == {"condition.compare", "condition.intent", "printf.bound"}, candidates
    for recipe, manifest in candidates.items():
        pending = json.loads((manifest.parent / "validation.json").read_text())
        assert pending["status"] == "not_run" and not pending["gates_passed"]
        data = run("validate-" + recipe, [cli, "repair", "validate", root, "--database", db,
                   "--output", out / "validation-runs", "--candidate", manifest,
                   "--c-compiler", cc, "--cxx-compiler", cxx, "--jobs", "2", "--timeout", "60", *coverage_args],
                   (0,) if recipe == "condition.compare" else (2,))
        value = json.loads(data)
        assert value["source_unchanged"] and value["correctness"] == "not_assessed", value
        assert value["baseline_target_reproduced"] and value["target_not_detected"], value
        assert value["status"] == ("gates_passed" if recipe == "condition.compare" else "tests_failed"), value
        if args.coverage:
            assert value["coverage"]["status"] == "collected" and value["coverage_commands_normalized"], value
        results[recipe] = value
    core_result = None
    if args.core_cli:
        core_result = json.loads(run("core-validation", [args.core_cli.resolve(strict=True), "repair", "validate", root,
                    "--database", db, "--output", out / "core-runs", "--candidate", candidates["condition.compare"],
                    "--c-compiler", cc, "--cxx-compiler", cxx, "--jobs", "2", "--timeout", "60", *coverage_args], (2,)))
        assert not core_result["gates_passed"] and core_result["analysis_status"] == "unavailable", core_result
    batch_result, core_batch = None, None
    if args.batch:
        command = [cli, "repair", "evaluate", root, "--database", db, "--output", out / "batch-runs",
                   "--c-compiler", cc, "--cxx-compiler", cxx, "--jobs", "2", "--timeout", "60", *coverage_args]
        for recipe in ("condition.intent", "printf.bound", "condition.compare"):
            command += ["--candidate", candidates[recipe]]
        batch_result = json.loads(run("batch-evaluate", command))
        assert batch_result["status"] == "completed" and batch_result["attempted"] == 3
        assert batch_result["gates_passed_count"] == 1 and not batch_result["all_gates_passed"]
        assert batch_result["entries"][0]["recipe"] == "condition.compare" and batch_result["entries"][0]["input_index"] == 2
        assert {e["recipe"]: e["changed_bytes"] for e in batch_result["entries"]}["condition.intent"] == 2
        assert batch_result["test_coverage"] == ("collected" if args.coverage else "not_measured") and batch_result["correctness"] == "not_assessed"
        if args.core_cli:
            core_batch = json.loads(run("core-batch", [args.core_cli.resolve(strict=True), "repair", "evaluate", root,
                    "--database", db, "--output", out / "core-batch-runs", "--candidate", candidates["condition.compare"],
                    "--c-compiler", cc, "--cxx-compiler", cxx, *coverage_args], (2,)))
            assert core_batch["status"] == "completed" and core_batch["gates_passed_count"] == 0
            assert core_batch["entries"][0]["validation"]["analysis_status"] == "unavailable"
    unexecuted = None
    if args.coverage:
        unused_root, unused_db = out / "unexecuted-source", out / "unexecuted.sqlite3"
        shutil.copytree(sample, unused_root)
        (unused_root / "main.cpp").write_text("int f(int n){if(n=0)return 1;return 0;}\nint main(){return 0;}\n", encoding="utf-8")
        unused_hashes = hashes(unused_root)
        run("unexecuted-configure", [toolbin / "cmake.exe", "-S", unused_root, "-B", out / "unexecuted-configure", "-G", "Ninja", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DCMAKE_CXX_COMPILER={cxx}"])
        run("unexecuted-scan", [cli, "scan", unused_root, "--database", unused_db, "--compile-commands", out / "unexecuted-configure"])
        proposal = run("unexecuted-propose", [cli, "repair", "propose", unused_root, "--database", unused_db, "--output", out / "unexecuted-candidates", "--rule", "CG003", "--file", "main.cpp", "--line", "1"])
        unused_manifest = next(row.split("\t")[2] for row in proposal.splitlines() if "\tcondition.compare\t" in row)
        unused_command = [cli, "repair", "validate", unused_root, "--database", unused_db, "--output", out / "unexecuted-runs", "--candidate", unused_manifest, "--cxx-compiler", cxx]
        unchecked = json.loads(run("unexecuted-without-coverage", unused_command))
        checked = json.loads(run("unexecuted-with-coverage", [*unused_command, *coverage_args], (2,)))
        assert unchecked["gates_passed"] and unchecked["coverage"]["status"] == "not_measured"
        assert not checked["gates_passed"] and checked["status"] == "coverage_target_uncovered"
        assert checked["coverage"]["target_line_status"] == "no_recorded_execution"
        assert hashes(unused_root) == unused_hashes
        unexecuted = {"without_coverage": unchecked, "with_coverage": checked, "all_original_files_unchanged": True, "source_sha256": unused_hashes}
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT MAX(id) FROM scan").fetchone()[0] == scan_id
        linked = list(connection.execute("SELECT id,scan_id,status,source_unchanged FROM build_run ORDER BY id"))
        assert all(r[1] == scan_id and r[3] == 1 for r in linked), linked
    assert hashes(root) == original == hashes(sample), "original or sample source changed"
    summary = {"passed": True, "synthetic_demo": True, "cli_sha256": hashlib.sha256(cli.read_bytes()).hexdigest(),
               "original_test_expected_failure": True, "original_steps": original_steps, "scan_id": scan_id,
               "candidates": {k: str(v) for k, v in candidates.items()}, "validation": results,
               "core_validation": core_result, "batch": batch_result, "core_batch": core_batch,
               "coverage_requested": args.coverage, "unexecuted_target": unexecuted,
               "build_runs": linked, "source_sha256": original,
               "all_original_files_unchanged": True, "correctness": "not_assessed"}
    save(out / "summary.json", summary)
    print("REPAIR_ACCEPTANCE_PASSED", flush=True)


if __name__ == "__main__":
    main()
