"""Exercise paired coverage using the delivered CLI; Python standard library only."""
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
            for p in sorted(root.rglob('*')) if p.is_file()}


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli', type=Path, required=True)
    p.add_argument('--core-cli', type=Path)
    p.add_argument('--toolchain', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cli = a.cli.resolve(strict=True)
    tool = a.toolchain.resolve(strict=True) / 'mingw64/bin'
    env = dict(os.environ, PATH=str(tool) + os.pathsep + os.environ['PATH'])
    root, db = out / 'source', out / 'scan.db'
    sample = REPO / 'samples/repair-comparison-demo'
    initial = hashes(sample)
    shutil.copytree(sample, root)
    commands = []

    def run(label, args, exits=(0,)):
        print('RUN', label, flush=True)
        args = [str(x) for x in args]
        result = subprocess.run(args, cwd=out, env=env, capture_output=True, timeout=300)
        (out / (label + '.log')).write_bytes(result.stdout + b'\n--- stderr ---\n' + result.stderr)
        commands.append(dict(label=label, args=args, exit_code=result.returncode))
        save(out / 'commands.json', commands)
        assert result.returncode in exits, (label, result.returncode)
        return result.stdout.decode('utf-8')

    run('configure', [tool / 'cmake.exe', '-S', root, '-B', out / 'cfg', '-G', 'Ninja',
                      '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', f'-DCMAKE_CXX_COMPILER={tool / "g++.exe"}'])
    run('scan', [cli, 'scan', root, '--database', db, '--compile-commands', out / 'cfg'])
    with sqlite3.connect(db) as con:
        issues = list(con.execute('SELECT rule_id,file,line,col FROM issue WHERE scan_id=(SELECT MAX(id) FROM scan) ORDER BY file'))
    assert len(issues) == 2 and all(i[0] == 'CG003' for i in issues), issues
    candidates = []
    for rule, file, line, col in issues:
        output = run('propose-' + file, [cli, 'repair', 'propose', root, '--database', db,
            '--output', out / 'candidates', '--rule', rule, '--file', file, '--line', line, '--column', col])
        candidates.append(next(Path(row.split('\t')[2]) for row in output.splitlines() if '\tcondition.compare\t' in row))
    output = run('compose', [cli, 'repair', 'compose', root, '--database', db, '--output', out / 'sets',
                           '--candidate', candidates[0], '--candidate', candidates[1]])
    manifest = Path(dict(row.split('=', 1) for row in output.splitlines() if '=' in row)['manifest'])
    common = ['repair', 'compare', root, '--database', db, '--output', out / 'comparisons',
              '--set', manifest, '--runs', '2', '--cxx-compiler', tool / 'g++.exe',
              '--gcov-tool', tool / 'gcov.exe', '--coverage-test-limit', '5', '--jobs', '2', '--timeout', '60']
    r = json.loads(run('compare', [cli, *common]))
    assert r['ready_for_review'] and r['comparison_complete'] and r['coverage_stable'] and r['tests_stable'], r
    assert r['correctness'] == 'not_assessed' and r['known_original_inputs_unchanged'], r
    assert [o['version'] for o in r['observations']] == ['original', 'patched', 'patched', 'original']
    assert len(r['groups']) == 6 and all(o['status'] == 'collected' for o in r['observations'])
    groups = {g['name']: g for g in r['groups']}
    transitions = lambda name: {l['transition'] for f in groups[name]['files'] if f['file'] == 'helpers.cpp' for l in f['lines']}
    assert 'newly_recorded' in transitions('zero [sample]') and 'no_longer_recorded' in transitions('one.sample')
    assert groups['one.sample']['selected_tests'] == ['cleanup', 'one.sample', 'ready']
    for o in r['observations']:
        if o['version'] == 'original':
            assert not o['standard_validation_passed'] and o['suite_steps'][-1]['tests_failed'] == 2
        else:
            assert o['standard_validation_passed']
    # Core can collect measurements but cannot certify static gates without Clang.
    core = None
    if a.core_cli:
        core = json.loads(run('core-compare', [a.core_cli.resolve(strict=True), *common], (2,)))
        assert core['comparison_complete'] and core['coverage_stable'] and not core['ready_for_review']
        assert not core['all_patched_validations_passed'] and core['correctness'] == 'not_assessed'
    assert hashes(root) == initial == hashes(sample), 'original inputs changed'
    save(out / 'acceptance.json', dict(status='passed', comparison=r, core_comparison=core,
                                      original_inputs_sha256=initial, commands=commands))
    print('REPAIR_COMPARISON_ACCEPTANCE_OK', flush=True)


if __name__ == '__main__':
    main()
