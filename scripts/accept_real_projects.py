"""Windows real-project acceptance. Python stdlib only; not a product dependency."""
import argparse
import collections
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sqlite3
import subprocess
import time
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


class MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("page_faults", ctypes.c_ulong)] + [
        (name, ctypes.c_size_t) for name in (
            "peak_working_set", "working_set", "peak_paged", "paged",
            "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")]


def run(command, cwd, log, env, allowed=(0,), timeout=900):
    command = [str(item) for item in command]
    print(f"RUN {log.stem}", flush=True)
    started, peak = time.perf_counter(), 0
    # File streams prevent a full pipe from blocking the monitored process.
    with log.open("wb") as stream:
        stream.write((subprocess.list2cmdline(command) + "\n").encode())
        stream.flush()
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        memory_info = ctypes.WinDLL("psapi").GetProcessMemoryInfo
        memory_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(MemoryCounters), ctypes.c_ulong]
        timed_out = False
        while process.poll() is None:
            counters = MemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if memory_info(int(process._handle), ctypes.byref(counters), counters.cb):
                peak = max(peak, counters.peak_working_set)
            if time.perf_counter() - started > timeout:
                subprocess.run([str(Path(env["SYSTEMROOT"]) / "System32/taskkill.exe"),
                                "/PID", str(process.pid), "/T", "/F"], capture_output=True)
                process.wait()
                timed_out = True
                break
            time.sleep(0.02)
        result = {"command": command, "exit_code": process.wait(), "timed_out": timed_out,
                  "wall_ms": round((time.perf_counter() - started) * 1000),
                  "peak_working_set_bytes_sampled": peak,
                  "memory_scope": "parent process only, sampled every 20ms; scan includes its worker threads"}
    save(log.with_suffix(".json"), result)
    if timed_out or result["exit_code"] not in allowed:
        raise RuntimeError(f"Command failed; inspect {log}")
    return result


def capture(command, cwd, env):
    return subprocess.check_output([str(x) for x in command], cwd=cwd, env=env)


def rows(connection, table, scan_id):
    return [dict(r) for r in connection.execute(f"SELECT * FROM {table} WHERE scan_id=?", (scan_id,))]


def analysis_report(database):
    with sqlite3.connect(database) as connection:
        connection.row_factory = sqlite3.Row
        analysis = dict(connection.execute("SELECT * FROM analysis ORDER BY scan_id DESC LIMIT 1").fetchone())
        scan_id = analysis["scan_id"]
        units = rows(connection, "translation_unit", scan_id)
        issues = rows(connection, "issue", scan_id)
        files = rows(connection, "file", scan_id)
        return {"analysis": analysis, "units": units, "issues": issues,
                "unit_status_counts": dict(collections.Counter(u["status"] for u in units)),
                "file_count": len(files), "physical_lines": sum(f["lines"] for f in files),
                "covered_files": len(rows(connection, "coverage", scan_id)),
                "issue_counts": dict(collections.Counter(i["rule_id"] for i in issues))}


def equivalent(one, four):
    def content(path):
        with sqlite3.connect(path) as con:
            con.row_factory = sqlite3.Row
            latest = con.execute("SELECT max(scan_id) FROM analysis").fetchone()[0]
            result = {}
            for table in ("translation_unit", "symbol", "function_metric", "graph_edge", "coverage", "issue"):
                result[table] = sorted(json.dumps({k: v for k, v in row.items() if k != "scan_id"}, sort_keys=True)
                                       for row in rows(con, table, latest))
            return result
    return content(one) == content(four)


def select_commands(source, raw, destination, preferences):
    commands = json.loads(raw.read_text(encoding="utf-8-sig"))
    groups = collections.defaultdict(list)
    for command in commands:
        file = Path(command["file"])
        if not file.is_absolute():
            file = Path(command["directory"]) / file
        groups[os.path.normcase(str(file.resolve()))].append(command)
    chosen, decisions = [], []
    for file, variants in sorted(groups.items()):
        def rank(command):
            text = json.dumps(command, sort_keys=True).replace("\\\\", "/")
            return next((i for i, target in enumerate(preferences) if target in text), len(preferences)), text
        ordered = sorted(variants, key=rank)
        chosen.append(ordered[0])
        if len(ordered) > 1:
            decisions.append({"file": file, "chosen": ordered[0], "other_configurations_not_analyzed": ordered[1:]})
    destination.parent.mkdir()
    save(destination, chosen)
    selection = {"raw_entries": len(commands), "selected_entries": len(chosen),
                 "policy": "prefer locked production targets, then stable lexical command order; analyze one configuration per file",
                 "preferred_targets": preferences, "decisions": decisions}
    save(destination.parent / "selection.json", selection)
    return groups, selection


def accept(project, args, env):
    out = args.output / project["id"]
    out.mkdir()
    source = out / "source"
    git = args.git
    if args.source_cache:
        run([git, "-c", "core.autocrlf=false", "clone", "--no-hardlinks", args.source_cache / project["id"], source],
            out, out / "clone.log", env)
    else:
        run([git, "init", source], out, out / "init.log", env)
        run([git, "-C", source, "remote", "add", "origin", project["repository"]], out, out / "remote.log", env)
        run([git, "-C", source, "fetch", "--depth", "1", "origin", project["commit"]], out, out / "fetch.log", env)
    run([git, "-C", source, "-c", "core.autocrlf=false", "checkout", "--detach", project["commit"]], out, out / "checkout.log", env)
    revision = capture([git, "rev-parse", "HEAD"], source, env).decode().strip()
    license_data = capture([git, "show", "HEAD:" + project["license_file"]], source, env)
    if revision != project["commit"] or digest(license_data) != project["license_sha256"]:
        raise RuntimeError("Upstream commit/license differs from lock")
    (out / "upstream-LICENSE.txt").write_bytes(license_data)
    tracked = capture([git, "ls-files", "-z"], source, env).decode().split("\0")
    before = {name: digest((source / name).read_bytes()) for name in tracked if name and (source / name).is_file()}
    cmake = args.bin / "cmake.exe"
    c_compiler = args.bin / project.get("c_compiler", "clang.exe")
    common = ["-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
              f"-DCMAKE_C_COMPILER={c_compiler}", f"-DCMAKE_CXX_COMPILER={args.bin / 'clang++.exe'}"]
    configure = run([cmake, "-S", source, "-B", out / "configure", "-G", "Ninja", *common,
                     *("-D" + value for value in project["definitions"])], out, out / "configure.log", env)
    raw_database = out / "raw.sqlite3"
    raw = run([args.cli, "scan", source, "--database", raw_database, "--compile-commands", out / "configure", "--threads", "4"],
              out, out / "raw-scan.log", env, allowed=(0, 2))
    raw_report = analysis_report(raw_database)
    save(out / "raw-analysis.json", raw_report)
    commands, selection = select_commands(source, out / "configure/compile_commands.json",
                                          out / "selected/compile_commands.json", project["preferred_targets"])
    scans = {}
    for threads in (1, 4):
        database = out / f"threads-{threads}.sqlite3"
        scans[str(threads)] = run([args.cli, "scan", source, "--database", database, "--compile-commands", out / "selected", "--threads", threads],
                                 out, out / f"scan-{threads}.log", env, allowed=(0, 2))
    report = analysis_report(out / "threads-4.sqlite3")
    save(out / "analysis.json", report)
    equality = equivalent(out / "threads-1.sqlite3", out / "threads-4.sqlite3")
    active_units = [unit for unit in report["units"] if os.path.normcase(str((source / unit["file"]).resolve())) in commands]
    compiled_ok = len(active_units) == len(commands) and bool(active_units) and all(unit["status"] == "success" for unit in active_units)
    review = json.loads((REPO / "config/real-project-review.json").read_text(encoding="utf-8-sig"))["projects"][project["id"]]
    expected = {(review["rule"], item["file"], item["line"]) for item in review["locations"]}
    actual = {(item["rule_id"], item["file"], item["line"]) for item in report["issues"]}
    reviewed = review["commit"] == revision and expected == actual and len(expected) == len(report["issues"])
    database = out / "threads-4.sqlite3"
    build = run([args.cli, "build", source, "--database", database, "--output", out / "runs", "--jobs", "4", "--timeout", "300",
                 "--c-compiler", c_compiler, "--cxx-compiler", args.bin / "clang++.exe",
                 *(item for value in project["definitions"] for item in ("--cmake-define", value)),
                 *(item for value in project["copy_includes"] for item in ("--copy-include", value))],
                out, out / "build.log", env, allowed=(0, 2), timeout=1000)
    run([args.cli, "query", source, "--database", database, "--query", "SELECT stage, status, tests_total, tests_failed FROM builds;"],
        out, out / "query-builds.log", env, allowed=(0, 2))
    with sqlite3.connect(database) as con:
        con.row_factory = sqlite3.Row
        build_run = dict(con.execute("SELECT * FROM build_run ORDER BY id DESC LIMIT 1").fetchone())
        build_run.pop("git_log")
        steps = [dict(row) for row in con.execute("SELECT name,status,exit_code,duration_ms,tests_total,tests_failed,tests_skipped FROM build_step WHERE run_id=? ORDER BY ordinal", (build_run["id"],))]
    test_file = Path(build_run["workspace"]) / "tests.xml"
    tests = ET.parse(test_file).getroot().attrib if test_file.exists() else {}
    source_unchanged = before == {name: digest((source / name).read_bytes()) for name in before}
    passed = (compiled_ok and equality and reviewed and source_unchanged and build_run["status"] == "passed"
              and int(tests.get("tests", 0)) >= project["minimum_tests"] and int(tests.get("failures", -1)) == 0
              and int(tests.get("skipped", -1)) == 0 and int(tests.get("disabled", -1)) == 0)
    summary = {"project": project, "source": str(source), "configure": configure, "raw_scan": raw,
               "raw_unit_status_counts": raw_report["unit_status_counts"], "selection": selection,
               "scans": scans, "analysis": {k: v for k, v in report.items() if k not in ("units", "issues")},
               "active_translation_units": len(active_units), "active_translation_units_success": sum(u["status"] == "success" for u in active_units),
               "equivalent_1_and_4_threads": equality, "build": build, "build_run": build_run,
               "build_steps": steps, "ctest": tests, "tracked_files_unchanged": source_unchanged,
               "reviewed_issue_locations_match": reviewed,
               "automated_acceptance_passed": passed}
    save(out / "summary.json", summary)
    print(f"RESULT {project['id']}: automated={passed}, issues={len(report['issues'])}, units={report['unit_status_counts']}", flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-cache", type=Path, help="Optional directory of fixed upstream Git clones")
    parser.add_argument("--projects", nargs="*", choices=("cjson", "tinyxml2", "fmt"))
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This acceptance profile is Windows-only")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    args.cli, args.toolchain = args.cli.resolve(strict=True), args.toolchain.resolve(strict=True)
    args.bin = args.toolchain / "mingw64/bin"
    if args.source_cache:
        args.source_cache = args.source_cache.resolve(strict=True)
    args.git = shutil.which("git.exe")
    if not args.git:
        raise RuntimeError("Git is required")
    lock = json.loads((REPO / "config/real-projects.lock.json").read_text(encoding="utf-8-sig"))
    tool_lock = json.loads((REPO / "config/windows-toolchain.lock.json").read_text(encoding="utf-8-sig"))
    if json.loads((args.toolchain / "toolchain.lock.json").read_text(encoding="utf-8-sig")) != tool_lock:
        raise RuntimeError("Toolchain lock differs from repository")
    env = os.environ.copy()
    for key in list(env):
        if key.upper().startswith(("CMAKE_", "QT_")) or key.upper() in (
                "CC", "CXX", "CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LIBRARY_PATH",
                "INCLUDE", "LIB", "CFLAGS", "CXXFLAGS", "LDFLAGS", "LLVM_DIR", "CLANG_DIR"):
            del env[key]
    env["PATH"] = os.pathsep.join(map(str, (args.bin, Path(args.git).parent, Path(env["SYSTEMROOT"]) / "System32", env["SYSTEMROOT"])))
    metadata = {"timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "os": platform.platform(),
                "python": platform.python_version(), "cli_sha256": digest(args.cli.read_bytes()),
                "git_commit": capture([args.git, "rev-parse", "HEAD"], REPO, env).decode().strip(),
                "working_diff_sha256": digest(capture([args.git, "diff", "HEAD"], REPO, env)),
                "project_lock_sha256": digest((REPO / "config/real-projects.lock.json").read_bytes()),
                "review_sha256": digest((REPO / "config/real-project-review.json").read_bytes()),
                "runner_sha256": digest(Path(__file__).read_bytes()),
                "toolchain": str(args.toolchain), "note": "Single local run per thread count; warm caches; no cross-machine performance claim"}
    save(args.output / "environment.json", metadata)
    results = []
    for project in lock["projects"]:
        if args.projects and project["id"] not in args.projects:
            continue
        try:
            result = accept(project, args, env)
        except Exception as error:
            result = {"project": project, "automated_acceptance_passed": False, "error": str(error)}
            print(f"FAILED {project['id']}: {error}", flush=True)
        results.append(result)
        save(args.output / "results.json", results)
    return 0 if results and all(item["automated_acceptance_passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
