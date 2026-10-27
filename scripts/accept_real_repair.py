"""Replay a pinned upstream bug, reference and generated repair; no Python packages required."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sqlite3
import subprocess
import zipfile
import xml.etree.ElementTree as ET
from real_repair_reference import reference_function
from real_repair_process import run_logged

REPO = Path(__file__).resolve().parents[1]
CASE = REPO / 'benchmarks/libgit2-error-return'


def save(p, value):
    p.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def hashes(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--repository', type=Path, help='Existing libgit2 Git object store; does not change its checkout')
    source.add_argument('--archives', type=Path, help='Offline folder containing the checksum-pinned buggy.zip and fixed.zip')
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New short ASCII path outside analyzed source')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--resume-prepared', action='store_true', help='Use verified existing preparation and start a new analysis attempt')
    parser.add_argument('--proposal-helper', type=Path, help='Benchmark driver using the existing multi-issue Core API in one parse')
    parser.add_argument('--reuse-scan', type=Path, help='Copy an already completed scan of these exact prepared inputs; generation still reparses the full scope')
    parser.add_argument('--reuse-proposals', type=Path, help='Replay saved proposals for identical saved scan, analyzer and C/C++ inputs; composition and full validation still check them')
    args = parser.parse_args()
    if args.proposal_helper:
        args.proposal_helper = args.proposal_helper.resolve(strict=True)
    if args.reuse_scan:
        args.reuse_scan = args.reuse_scan.resolve(strict=True)
    if args.reuse_proposals:
        args.reuse_proposals = args.reuse_proposals.resolve(strict=True)
    cli, source_repo, tool = args.cli.resolve(strict=True), args.repository.resolve(strict=True) if args.repository else None, args.toolchain.resolve(strict=True) / 'mingw64/bin'
    archives = args.archives.resolve(strict=True) if args.archives else None
    out = args.output.resolve()
    if not args.resume_prepared:
        out.mkdir(parents=True, exist_ok=False)
    if not str(out).isascii():
        raise ValueError('use a short ASCII benchmark output path')
    case = json.loads((CASE / 'manifest.json').read_text(encoding='utf-8'))
    env = dict(os.environ, PATH=str(tool) + os.pathsep + os.environ['PATH'])
    commands = json.loads((out / 'commands.json').read_text(encoding='utf-8')) if args.resume_prepared else []

    def run(label, command, exits=(0,), cwd=out, timeout=300):
        print('RUN', label, flush=True)
        return run_logged(label, command, out, commands, env=env, cwd=cwd, timeout=timeout, exits=exits)

    defs = ['BUILD_TESTS=OFF', 'BUILD_CLI=OFF', 'BUILD_SHARED_LIBS=OFF', 'BUILD_EXAMPLES=OFF', 'BUILD_FUZZERS=OFF',
            'USE_HTTPS=OFF', 'USE_SSH=OFF', 'USE_COMPRESSION=builtin', 'USE_REGEX=builtin', 'USE_AUTH_NTLM=OFF', 'USE_AUTH_NEGOTIATE=OFF',
            'CODEGUARD_BENCHMARK_REPO_ROOT=' + (out / 'repos').as_posix()]
    records = json.loads((out / 'preparation.json').read_text(encoding='utf-8'))['versions'] if args.resume_prepared else {}
    for label, ref in [('buggy', case['buggy_commit']), ('fixed', case['fixed_commit'])]:
        if label in records:
            assert records[label]['upstream_ref'] == ref and hashes(Path(records[label]['root'])) == records[label]['prepared_sha256']
            continue
        archive = out / (label + '.zip')
        if archives:
            expected = json.loads((CASE / 'archive-sha256.json').read_text(encoding='utf-8'))[label]
            data = (archives / (label + '.zip')).read_bytes()
            assert len(data) == expected['bytes'] and hashlib.sha256(data).hexdigest() == expected['sha256'] and expected['git_commit'] == ref
            archive.write_bytes(data)
        else:
            actual = run(label + '-ref', ['git', '-C', source_repo, 'rev-parse', ref + '^{commit}']).strip()
            assert actual == ref
            with archive.open('wb') as stream:
                p = subprocess.run(['git', '-C', str(source_repo), 'archive', '--format=zip', ref], env=env, stdout=stream, stderr=subprocess.PIPE, timeout=180)
            (out / (label + '-archive.log')).write_bytes(p.stderr)
            assert p.returncode == 0 and archive.stat().st_size < 100 * 1024 * 1024
        root = out / ('s' if label == 'buggy' else 'f')
        root.mkdir()
        omitted = []
        with zipfile.ZipFile(archive) as z:
            assert z.testzip() is None and z.comment.decode('ascii') == ref
            for item in z.infolist():
                path = PurePosixPath(item.filename)
                if path.is_absolute() or '..' in path.parts or '\\' in item.filename:
                    raise ValueError('unsafe archive path')
                if path.parts[0] in ('tests', 'examples', 'fuzzers', 'benchmarks', 'ci', '.github', 'docs'):
                    omitted.append(item.filename)
                    continue
                if ((item.external_attr >> 16) & 0o170000) == 0o120000:
                    raise ValueError('source symlink unsupported')
                destination = root.joinpath(*path.parts)
                if item.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                else:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(z.read(item))
        original = hashes(root)
        shutil.copyfile(CASE / 'regression.c', root / 'codeguard-regression.c')
        shutil.copyfile(CASE / 'harness.cmake', root / 'codeguard-harness.cmake')
        with (root / 'CMakeLists.txt').open('a', encoding='utf-8') as stream:
            stream.write('\ninclude("${CMAKE_CURRENT_SOURCE_DIR}/codeguard-harness.cmake")\n')
        cfg = out / ('b' if label == 'buggy' else 'fb')
        configure = [tool / 'cmake.exe', '-S', root, '-B', cfg, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
                     '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', f'-DCMAKE_C_COMPILER={tool / "gcc.exe"}'] + ['-D' + d for d in defs]
        run(label + '-configure', configure)
        run(label + '-build', [tool / 'cmake.exe', '--build', cfg, '--parallel', '4'])
        junit = out / (label + '-tests.xml')
        run(label + '-tests', [tool / 'ctest.exe', '--test-dir', cfg, '--output-on-failure', '--output-junit', junit], (8,) if label == 'buggy' else (0,))
        records[label] = dict(root=str(root), cfg=str(cfg), upstream_ref=ref, original_sha256=original, prepared_sha256=hashes(root), omitted=omitted)
        save(out / 'preparation.json', dict(case=case, definitions=defs, versions=records))
    if args.prepare_only:
        print('REAL_REPAIR_PREPARED', flush=True)
        return
    def test_results(path):
        cases = list(ET.parse(path).getroot().iter('testcase'))
        return {t.get('name'): 'failed' if t.find('failure') is not None else 'skipped' if t.find('skipped') is not None else 'passed' for t in cases}
    expected = {'codeguard-real.' + n for n in ('tree-invalid-options', 'index-invalid-options', 'merge-corrupt-index', 'valid-options-control')}
    baseline, fixed_tests = test_results(out / 'buggy-tests.xml'), test_results(out / 'fixed-tests.xml')
    assert set(baseline) == set(fixed_tests) == expected
    assert all(v == ('passed' if n.endswith('valid-options-control') else 'failed') for n, v in baseline.items())
    assert all(v == 'passed' for v in fixed_tests.values())
    for n in range(100):
        attempt = out / ('a' + str(n))
        if not attempt.exists():
            attempt.mkdir()
            break
    else:
        raise RuntimeError('too many recorded attempts')
    prefix = attempt.name + '-'
    root, cfg, db = Path(records['buggy']['root']), Path(records['buggy']['cfg']), attempt / 'scan.db'
    entries = json.loads((cfg / 'compile_commands.json').read_text(encoding='utf-8'))
    used = {Path(e['file']).resolve() for e in entries}
    excluded = sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.suffix in ('.c', '.cc', '.cpp', '.cxx') and p.resolve() not in used)
    save(attempt / 'scope.json', dict(compile_commands=len(entries), uncompiled_cpp_excluded=excluded,
                                    note='Selected Windows library configuration plus CodeGuard public-API tests; not full repository coverage'))
    arguments = [cli, 'config', root, '--database', db, '--compile-commands', cfg,
                 '--c-compiler', tool / 'gcc.exe', '--jobs', '4', '--timeout', '600', '--threads', '4']
    for d in defs:
        arguments.extend(['--cmake-define', d])
    for path in excluded:
        arguments.extend(['--copy-exclude', path])
    if args.reuse_scan:
        with sqlite3.connect(args.reuse_scan) as original, sqlite3.connect(db) as copied:
            original.backup(copied)
        save(attempt / 'reused-scan.json', dict(source=str(args.reuse_scan), note='Completed snapshot copied; prepared inputs verified. Proposal replay, when requested, does not run generation; full validation still reparses baseline and patched TU scope.'))
    run(prefix + 'config', arguments)
    if not args.reuse_scan:
        run(prefix + 'scan', [cli, 'scan', root, '--database', db], timeout=900)
    with sqlite3.connect(db) as connection:
        scan_id = connection.execute('SELECT MAX(id) FROM scan').fetchone()[0]
        selected = []
        for target in case['targets']:
            rows = list(connection.execute('SELECT rule_id,file,line,col FROM issue WHERE scan_id=? AND rule_id=? AND file=? AND line=?',
                                          (scan_id, case['rule'], target['file'], target['line'])))
            assert len(rows) == 1, (target, rows)
            selected.append(rows[0])
    manifests = {'condition.assignment_result': [], 'condition.intent': []}
    if args.reuse_proposals:
        rows = args.reuse_proposals.read_text(encoding='utf-8').splitlines()
        save(attempt / 'reused-proposals.json', dict(source=str(args.reuse_proposals), sha256=hashlib.sha256(args.reuse_proposals.read_bytes()).hexdigest(),
             note='Saved proposals replayed; composition enforces saved scan, configuration, analyzer and exact edit inputs. Full copy validation, baseline and patched analysis still run; proposal generation itself is not repeated.'))
    elif args.proposal_helper:
        rows = run(prefix + 'propose-all', [args.proposal_helper, root, db, attempt / 'p', *[str(v) for row in selected for v in row]], timeout=900).splitlines()
    else:
        rows = []
        for index, (rule, file, line, column) in enumerate(selected):
            rows += run(prefix + 'propose-' + str(index), [cli, 'repair', 'propose', root, '--database', db, '--output', attempt / 'p',
                        '--rule', rule, '--file', file, '--line', line, '--column', column], timeout=900).splitlines()
    for row in rows:
        fields = row.split('\t')
        if len(fields) == 3 and fields[1] in manifests:
            manifests[fields[1]].append(Path(fields[2]))
    assert all(len(items) == 4 for items in manifests.values())
    validations = {}
    for recipe, items in manifests.items():
        name = 'fix' if recipe == case['recipe'] else 'intent'
        args_compose = [cli, 'repair', 'compose', root, '--database', db, '--output', attempt / 'sets']
        for item in items:
            args_compose.extend(['--candidate', item])
        composed = run(prefix + name + '-compose', args_compose)
        manifest = Path(dict(row.split('=', 1) for row in composed.splitlines() if '=' in row)['manifest'])
        build_args = [item for d in defs for item in ('--cmake-define', d)]
        value = json.loads(run(prefix + name + '-validate', [cli, 'repair', 'validate-set', root, '--database', db,
                              '--output', attempt / 'v', '--set', manifest, *build_args], (0,) if name == 'fix' else (2,), timeout=1800))
        with sqlite3.connect(db) as connection:
            value['benchmark_build_workspace'] = connection.execute('SELECT workspace FROM build_run WHERE id=?', (value['build_run_id'],)).fetchone()[0]
        validations[name] = value
        save(attempt / (name + '-validation.json'), value)
        assert value['source_unchanged'] and value['proposed_copy_verified'] and value['correctness'] == 'not_assessed'
        assert value['targets'] and all(t['target_not_detected'] for t in value['targets'])
        assert value['tests_total'] == 4 and value['tests_failed'] == (0 if name == 'fix' else 3)
        assert value['gates_passed'] == (name == 'fix')
    functions = sorted({(t['file'], t['function']) for t in case['targets']})
    ast_dir = attempt / 'reference-ast'
    ast_dir.mkdir()
    ast = {}
    for version in ('buggy', 'fixed', 'fix', 'intent'):
        if version in records:
            r, b = Path(records[version]['root']), Path(records[version]['cfg'])
        else:
            work = Path(validations[version]['benchmark_build_workspace'])
            r, b = work / 'source', work / 'build'
        ast[version] = [reference_function(r, b, file, function, tool / 'clang.exe', ast_dir / (version + '-' + function), env)
                        for file, function in functions]
    digest = lambda version: [d['sha256'] for d in ast[version]]
    assert all(a != b for a, b in zip(digest('buggy'), digest('fixed'))), 'oracle did not observe upstream condition changes'
    assert digest('fix') == digest('fixed'), 'candidate typed function AST differs from upstream reference'
    assert digest('intent') == digest('buggy') and digest('intent') != digest('fixed'), 'intent is not a valid negative control'
    for version, data in records.items():
        assert hashes(Path(data['root'])) == data['prepared_sha256'], version + ' original inputs changed'
    result = dict(status='passed', case=case, scope=json.loads((attempt / 'scope.json').read_text(encoding='utf-8')),
                  native_baseline_tests=baseline, native_reference_tests=fixed_tests, validations=validations, typed_ast=ast,
                  reference_functions_matched=3, independently_reproduced_failure_sites=3, reference_edit_sites=4,
                  correctness='not_assessed', original_inputs_unchanged=True, commands=commands)
    save(attempt / 'acceptance.json', result)
    save(out / 'latest-acceptance.json', dict(attempt=attempt.name, report=str(attempt / 'acceptance.json')))
    print('REAL_REPAIR_ACCEPTANCE_OK', flush=True)


if __name__ == '__main__':
    main()
