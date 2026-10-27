"""Exercise explicit repair sets through the delivered CLI (Python stdlib only)."""
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
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--coverage", action="store_true")
    parser.add_argument("--per-test", action="store_true", help="Use the five-test fixture demo and isolated gcov replay")
    args = parser.parse_args()
    sample = REPO / ("samples/repair-test-demo" if args.per_test else "samples/repair-set-demo")
    out, cli = args.output.resolve(), args.cli.resolve(strict=True)
    if out.is_relative_to(sample):
        raise ValueError("output must be outside original sample")
    out.mkdir(parents=True, exist_ok=False)
    toolbin = args.toolchain.resolve(strict=True) / "mingw64/bin"
    env = os.environ.copy()
    env["PATH"] = str(toolbin) + os.pathsep + env["PATH"]
    root, db = out / "source", out / "scan.sqlite3"
    original = hashes(sample)
    shutil.copytree(sample, root)
    commands = []

    def run(label, command, exits=(0,)):
        command = [str(x) for x in command]
        print("RUN", label, flush=True)
        result = subprocess.run(command, cwd=out, env=env, capture_output=True, timeout=180)
        (out / (label + ".log")).write_bytes(result.stdout + b"\n--- stderr ---\n" + result.stderr)
        commands.append(dict(label=label, command=command, exit_code=result.returncode))
        save(out / "commands.json", commands)
        if result.returncode not in exits:
            raise RuntimeError(f"{label}: exit {result.returncode}; inspect log")
        return result.stdout.decode("utf-8")

    cc, cxx = toolbin / "gcc.exe", toolbin / "g++.exe"
    configure = out / "configure"
    run("configure", [toolbin / "cmake.exe", "-S", root, "-B", configure, "-G", "Ninja",
                      "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DCMAKE_CXX_COMPILER={cxx}"])
    run("scan", [cli, "scan", root, "--database", db, "--compile-commands", configure])
    with sqlite3.connect(db) as connection:
        scan_id = connection.execute("SELECT MAX(id) FROM scan").fetchone()[0]
        issues = list(connection.execute("SELECT rule_id,file,line,col FROM issue WHERE scan_id=? ORDER BY file", (scan_id,)))
    assert len(issues) == 2 and all(i[0] == "CG003" for i in issues), issues
    candidates = {}
    for rule, file, line, column in issues:
        output = run("propose-" + file, [cli, "repair", "propose", root, "--database", db,
                     "--output", out / "candidates", "--rule", rule, "--file", file,
                     "--line", line, "--column", column])
        candidates[file] = {f[1]: Path(f[2]) for row in output.splitlines()
                            if len(f := row.split("\t")) == 3}
        assert set(candidates[file]) == {"condition.compare", "condition.intent"}, candidates
    selected = [candidates[file]["condition.compare"] for file in sorted(candidates)]
    build_args = ["--c-compiler", cc, "--cxx-compiler", cxx, "--jobs", "2", "--timeout", "60"]
    coverage_args = ["--coverage", "gcov-tests" if args.per_test else "gcov", "--gcov-tool", toolbin / "gcov.exe"] if args.coverage or args.per_test else []
    singles = []
    for index, manifest in enumerate(selected):
        value = json.loads(run(f"single-{index}", [cli, "repair", "validate", root, "--database", db,
                               "--output", out / "singles", "--candidate", manifest, *build_args], (2,)))
        assert value["status"] == "tests_failed" and not value["gates_passed"], value
        assert value["source_unchanged"] and value["proposed_copy_verified"], value
        singles.append(value)

    def compose(label, parts):
        arguments = [cli, "repair", "compose", root, "--database", db, "--output", out / "sets"]
        for part in parts:
            arguments.extend(["--candidate", part])
        output = run(label, arguments)
        fields = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
        return fields["set_id"], Path(fields["manifest"])

    set_id, manifest = compose("compose", selected)
    reverse_id, reverse = compose("compose-reversed", list(reversed(selected)))
    assert set_id == reverse_id and manifest.read_bytes() == reverse.read_bytes()
    pending = json.loads((manifest.parent / "validation.json").read_text())
    assert pending["status"] == "not_run" and len(pending["targets"]) == 2
    value = json.loads(run("validate-set", [cli, "repair", "validate-set", root, "--database", db,
                           "--output", out / "validation", "--set", manifest, *build_args, *coverage_args]))
    assert value["gates_passed"] and value["source_unchanged"] and value["proposed_copy_verified"], value
    assert value["correctness"] == "not_assessed" and value["new_or_unmasked_issues"] == 0, value
    assert value["tests_total"] == (5 if args.per_test else 1) and value["tests_failed"] == value["tests_skipped"] == 0, value
    assert len(value["targets"]) == 2 and all(t["baseline_reproduced"] and t["target_not_detected"] for t in value["targets"]), value
    if args.coverage or args.per_test:
        assert value["coverage"]["status"] == "collected" and all(t["coverage_line_status"] == "recorded" for t in value["targets"]), value
    if args.per_test:
        replay = value["test_coverage"]
        assert replay["status"] == "collected" and replay["line_union_matches_aggregate"] and len(replay["entries"]) == 5, replay
        entries = {e["name"]: e for e in replay["entries"]}
        targets = lambda name: {t["file"]: t["status"] for t in entries[name]["targets"]}
        assert targets("zero [sample]") == {"zero.cpp": "recorded", "one.cpp": "no_recorded_execution"}, replay
        assert targets("one.sample") == {"one.cpp": "recorded", "zero.cpp": "no_recorded_execution"}, replay
        assert set(entries["one.sample"]["selected_tests"]) == {"one.sample", "ready", "cleanup"}, replay
        assert all(v == "no_recorded_execution" for v in targets("idle").values()), replay
    # Two rules disappear even when one behavioral change is still wrong.
    bad_parts = [selected[0], candidates[sorted(candidates)[1]]["condition.intent"]]
    _, bad_manifest = compose("compose-behavioral-counterexample", bad_parts)
    bad = json.loads(run("reject-behavioral-counterexample", [cli, "repair", "validate-set", root,
                         "--database", db, "--output", out / "rejected", "--set", bad_manifest, *build_args], (2,)))
    assert bad["status"] == "tests_failed" and bad["target_not_detected"] and not bad["gates_passed"], bad
    first = candidates[sorted(candidates)[0]]
    run("reject-alternatives", [cli, "repair", "compose", root, "--database", db, "--output", out / "conflict",
                               "--candidate", first["condition.compare"], "--candidate", first["condition.intent"]], (1, 2))
    core = None
    if args.core_cli:
        core = json.loads(run("core-analysis-unavailable", [args.core_cli.resolve(strict=True), "repair", "validate-set", root,
                              "--database", db, "--output", out / "core", "--set", manifest, *build_args, *(coverage_args if args.per_test else [])], (2,)))
        assert core["status"] == "analysis_unavailable" and core["build_status"] == core["test_status"] == "passed" and not core["gates_passed"], core
        if args.per_test:
            assert core["test_coverage"]["status"] == "collected" and core["test_coverage"]["line_union_matches_aggregate"], core
    # Mix both recipe families in one line; an existing array finding inside
    # sprintf's retained argument must map back across both inserted fragments.
    mixed_root, mixed_db = out / "mixed-source", out / "mixed.sqlite3"
    mixed_root.mkdir()
    (mixed_root / "main.cpp").write_text(
        '#include <cstdio>\nint f(int flag){int a[2]={};char b[8];if(flag)std::sprintf(b,"%d",a[2]);int n=1;if(n=0)return 1;return 0;}\nint main(){return f(0);}\n', encoding="utf-8")
    (mixed_root / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(MixedSet LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp)\nadd_test(NAME safe_path COMMAND demo)\n', encoding="utf-8")
    mixed_hashes = hashes(mixed_root)
    run("mixed-configure", [toolbin / "cmake.exe", "-S", mixed_root, "-B", out / "mixed-configure", "-G", "Ninja",
                            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DCMAKE_CXX_COMPILER={cxx}"])
    run("mixed-scan", [cli, "scan", mixed_root, "--database", mixed_db, "--compile-commands", out / "mixed-configure"])
    parts = []
    for rule, recipe in (("CG001", "printf.bound"), ("CG003", "condition.compare")):
        output = run("mixed-propose-" + rule, [cli, "repair", "propose", mixed_root, "--database", mixed_db,
                     "--output", out / "mixed-candidates", "--rule", rule, "--file", "main.cpp", "--line", "2"])
        parts.append(next(row.split("\t")[2] for row in output.splitlines() if "\t" + recipe + "\t" in row))
    mixed_output = run("mixed-compose", [cli, "repair", "compose", mixed_root, "--database", mixed_db,
                       "--output", out / "mixed-sets", "--candidate", parts[0], "--candidate", parts[1]])
    mixed_manifest = next(row.split("=", 1)[1] for row in mixed_output.splitlines() if row.startswith("manifest="))
    mixed = json.loads(run("mixed-validate", [cli, "repair", "validate-set", mixed_root, "--database", mixed_db,
                           "--output", out / "mixed-validation", "--set", mixed_manifest, *build_args]))
    assert mixed["gates_passed"] and mixed["before_issues"] == 3 and mixed["after_issues"] == 1 and mixed["new_or_unmasked_issues"] == 0, mixed
    assert len(mixed["targets"]) == 2 and hashes(mixed_root) == mixed_hashes and mixed["proposed_copy_verified"], mixed
    assert hashes(root) == original == hashes(sample), "original sample or source clone was modified"
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT MAX(id) FROM scan").fetchone()[0] == scan_id
        runs = list(connection.execute("SELECT id,scan_id,status FROM build_run ORDER BY id"))
    assert all(r[1] == scan_id for r in runs), runs
    evidence = dict(cli=str(cli), cli_sha256=hashlib.sha256(cli.read_bytes()).hexdigest(),
                    scan_id=scan_id, set_id=set_id, manifest=str(manifest), source_hashes=original,
                    source_unchanged=True, reverse_order_identical=True, singles=singles,
                    combined=value, behavioral_counterexample=bad, core=core, build_runs=runs,
                    mixed_recipe_mapping=mixed, mixed_source_hashes=mixed_hashes,
                    per_test_requested=args.per_test,
                    correctness="not_assessed")
    save(out / "result.json", evidence)
    print("REPAIR_SET_ACCEPTANCE_OK", flush=True)


if __name__ == "__main__":
    main()
