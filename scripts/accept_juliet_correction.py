"""Fixed-cohort, preregistered classification controls; no automatic expansion.

Run with PYTHONPATH=src and .venv-ml/Scripts/python.exe. Each phase is explicit;
existing model directories are preserved. The validation gate is an engineering
screen, not a significance test. Test results are descriptive after the decision.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

from defectguard.experiments.data import load_dataset, prepare_dataset, read_jsonl
from defectguard.experiments.juliet import import_juliet, sha256_file


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8', newline='\n')


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def command(args, log):
    result = subprocess.run([str(x) for x in args], capture_output=True, encoding='utf-8', errors='replace')
    log.write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {args}; see {log}')


def prepare(args):
    output = args.output
    import_juliet(args.archive, output, max_cases=120, selection=args.baseline / 'provenance.jsonl')
    source = output / 'corpus'
    command(['cmake', '-S', source, '-B', source / 'build', '-G', 'MinGW Makefiles',
             '-DCMAKE_C_COMPILER=' + args.compiler.as_posix()], output / 'cmake-configure.log')
    command(['cmake', '--build', source / 'build', '--target', 'juliet_cases', '--parallel', '4'], output / 'cmake-build.log')
    graphs(args)


def graphs(args):
    from defectguard.config import DEFAULT_CONFIG
    from defectguard.pipeline import AnalysisPipeline
    from defectguard.graphs import write_graphs
    output = args.output
    source = output / 'corpus'
    if (output / 'graphs').exists() or (output / 'dataset').exists():
        raise ValueError('Graph/dataset output already exists')
    config = replace(DEFAULT_CONFIG, parser_backend='clang', clang_tool=str(args.native.resolve()),
                     compilation_database='build/compile_commands.json', exclude=('build',))
    report = AnalysisPipeline(config).scan(source)
    write_graphs(report, output / 'graphs')
    prepare_dataset(output / 'graphs/graphs.jsonl', output / 'labels.jsonl', output / 'dataset', 42)
    current, splits = load_dataset(output / 'dataset')
    old, old_splits = load_dataset(args.baseline / 'dataset')
    assert {x['sample_id'] for x in current} == {x['sample_id'] for x in old}, 'Changed cohort'
    assert all(set(splits['splits'][k]) == set(old_splits['splits'][k]) for k in ('train','validation','test')), 'Changed split'
    assert report.source_unchanged and report.parser_backend == 'clang-libtooling'
    write(output / 'preparation.json', {'compiled': True, 'source_unchanged': True, 'same_cohort': True,
          'same_split': True, 'functions': report.metrics.function_count,
          'dataset_count': len(current), 'dataset_sha256': splits['dataset_sha256']})


def audit(args):
    import torch
    from defectguard.experiments.network import fit_vocabulary, tensor_graph
    output = args.output
    records, splits = load_dataset(output / 'dataset')
    train = set(splits['splits']['train'])
    collisions = {}
    for version in ('legacy-v1', 'semantic-v2'):
        vocab = fit_vocabulary([r for r in records if r['sample_id'] in train], version)
        buckets = defaultdict(list)
        for r in records:
            graph = tensor_graph(r, vocab, torch.zeros(768), feature_version=version)
            digest = hashlib.sha256()
            for value in (graph.kinds, graph.numeric, graph.edge_index, graph.edge_type):
                digest.update(str(tuple(value.shape)).encode()); digest.update(value.numpy().tobytes())
            buckets[digest.hexdigest()].append(r)
        conflicts = [rs for rs in buckets.values() if len({r['label'] for r in rs}) > 1]
        collisions[version] = {'groups': len(conflicts), 'functions': sum(len(rs) for rs in conflicts),
                               'sample_ids': [[r['sample_id'] for r in rs] for rs in conflicts]}
    clues = [r['sample_id'] for r in records if re.search(r'good|bad|CWE\d+', r['source'], re.I)]
    origins = read_jsonl(output / 'provenance.jsonl')
    for item in origins:
        assert sha256_file(output / 'corpus' / item['file']) == item['generated_file_sha256']
        original = output / 'originals' / (hashlib.sha256(item['source_file'].encode()).hexdigest() + '.c')
        assert sha256_file(original) == item['original_sha256']
    write(output / 'feature-audit.json', {'answer_clues': clues, 'feature_collisions': collisions,
          'provenance_hashes_verified': len(origins), 'renamed_functions': sum(bool(x['renamed_identifiers']) for x in origins)})
    assert not clues
    assert collisions['semantic-v2']['groups'] == 0, 'Conflicting identical model inputs remain'


def plan(args):
    output = args.output
    if (output / 'plan.json').exists():
        raise ValueError('Plan already exists')
    _, splits = load_dataset(output / 'dataset')
    runs = []
    for seed in (42,43,44):
        for version in ('legacy-v1','semantic-v2'):
            runs.append({'name': f'gnn-{version}-{seed}', 'mode': 'gnn', 'feature_version': version, 'max_length':512, 'seed':seed})
        runs.append({'name': f'fusion-semantic-v2-{seed}', 'mode':'fusion','feature_version':'semantic-v2','max_length':512,'seed':seed})
    for length in (256,512):
        runs.append({'name': f'sequence-{length}-42', 'mode':'sequence','feature_version':'semantic-v2','max_length':length,'seed':42})
    write(output / 'plan.json', {'dataset_sha256':splits['dataset_sha256'], 'splits_sha256':sha256_file(output/'dataset/splits.json'),
          'epochs':40,'learning_rate':0.003,'localization_weight':0.0,'threshold':0.5,'runs':runs,
          'gate':{'mean_fusion_validation_balanced_accuracy_min':0.6, 'noncollapsed_fusion_validation_seeds_min':2},
          'gate_scope':'engineering screen only, no test tuning, no significance claim'})


def train(args):
    from defectguard.experiments.runner import train_experiment
    output = args.output
    spec = read(output / 'plan.json')
    assert sha256_file(output / 'dataset/dataset.jsonl') == spec['dataset_sha256']
    assert sha256_file(output / 'dataset/splits.json') == spec['splits_sha256']
    runs = [r for r in spec['runs'] if not args.run or r['name'] == args.run]
    if not runs:
        raise ValueError('Unknown run')
    for run in runs:
        print('REGISTERED RUN', run['name'], flush=True)
        train_experiment(output / 'dataset', output / run['name'], mode=run['mode'], feature_version=run['feature_version'],
                         encoder=args.encoder, cache=output/'embedding-cache', max_length=run['max_length'], seed=run['seed'],
                         epochs=spec['epochs'], learning_rate=spec['learning_rate'], threshold=spec['threshold'], localization_weight=spec['localization_weight'])


def summarize(args):
    from defectguard.experiments.runner import TrainedPredictor
    from defectguard.experiments.evaluation import evaluate_predictions
    output = args.output
    spec = read(output / 'plan.json')
    records, splits = load_dataset(output / 'dataset'); by = {r['sample_id']:r for r in records}
    summary = []
    for run in spec['runs']:
        root = output / run['name']; meta = read(root / 'metadata.json'); metrics = read(root / 'metrics.json')
        assert meta['dataset_sha256'] == spec['dataset_sha256'] and meta['splits_sha256'] == spec['splits_sha256']
        assert meta['localization_loss_weight'] == 0 and meta['epochs'] == spec['epochs']
        assert meta['seed'] == run['seed'] and meta['network']['mode'] == run['mode']
        assert meta['network'].get('feature_version', 'legacy-v1') == run['feature_version']
        assert meta['learning_rate'] == spec['learning_rate'] and meta['threshold'] == spec['threshold']
        if meta.get('encoding'):
            assert meta['encoding']['max_length'] == run['max_length']
        assert sha256_file(root / 'model.safetensors') == meta['weights_sha256']
        scores = {}
        for subset in ('train','validation','test'):
            preds = read(root / f'{subset}-predictions.json'); expected = set(splits['splits'][subset])
            assert len(preds) == len(expected) and {x['sample_id'] for x in preds} == expected
            assert evaluate_predictions([by[i] for i in expected], preds, spec['threshold']) == metrics[subset]
            pos = [x['probability'] for x in preds if by[x['sample_id']]['label']]
            neg = [x['probability'] for x in preds if not by[x['sample_id']]['label']]
            auc = sum((a>b) + .5*(a==b) for a in pos for b in neg) / (len(pos)*len(neg))
            confusion = metrics[subset]['confusion_matrix']
            positive_count = confusion['tp'] + confusion['fp']
            ba = .5*(confusion['tp']/len(pos) + confusion['tn']/len(neg))
            scores[subset] = {'f1':metrics[subset]['f1'],'balanced_accuracy':ba,'auc':auc,
                              'predicted_positive':positive_count,'collapsed':positive_count in (0,len(preds)),
                              'confusion_matrix':confusion}
        if run['name'] == 'fusion-semantic-v2-42':
            predictor = TrainedPredictor(root)
            assert predictor.predict([r for r in records if r['sample_id'] in splits['splits']['test']]) == read(root/'test-predictions.json')
        summary.append({'name':run['name'], 'best_epoch':meta['best_epoch'], 'scores':scores,
                        'truncated_functions':(meta.get('encoding') or {}).get('truncated_functions',0)})
    fusion = [x['scores']['validation'] for x in summary if x['name'].startswith('fusion-')]
    mean_ba = statistics.mean(x['balanced_accuracy'] for x in fusion)
    noncollapsed = sum(not x['collapsed'] for x in fusion)
    gate = mean_ba >= spec['gate']['mean_fusion_validation_balanced_accuracy_min'] and noncollapsed >= spec['gate']['noncollapsed_fusion_validation_seeds_min']
    write(output/'comparison.json', {'runs':summary,'fusion_validation_mean_balanced_accuracy':mean_ba,
          'fusion_validation_noncollapsed_seeds':noncollapsed, 'learning_gate_passed':gate,
          'expansion_authorized_by_gate':gate,'fusion_reload_exact':True,
          'note':'The existing test set informed development before this experiment and is diagnostic, not a fresh final holdout.'})
    print(json.dumps({'learning_gate_passed':gate,'mean_validation_balanced_accuracy':mean_ba,'noncollapsed_seeds':noncollapsed},indent=2),flush=True)


def scan(args):
    """Check the new graph feature version through the actual online backend."""
    from defectguard.experiments.runner import TrainedPredictor
    output = args.output
    target = output / 'online-scan'
    if target.exists():
        raise ValueError('Online scan already exists')
    checkpoint = output / 'gnn-semantic-v2-42'
    config = output / 'scan-semantic.toml'
    config.write_text('\n'.join([
        '[project]', 'exclude = ["build"]', '[parser]', 'backend = "clang"',
        'clang_tool = ' + json.dumps(args.native.resolve().as_posix()),
        'compilation_database = "build/compile_commands.json"', '[models]', 'backend = "trained"',
        'checkpoint = ' + json.dumps(checkpoint.resolve().as_posix()), '']), encoding='utf-8')
    command([sys.executable, '-m', 'defectguard', 'scan', output/'corpus', '--config',config,'--output',target], output/'online-scan.log')
    report = read(target / 'report.json')
    predictions = TrainedPredictor(checkpoint).predict(read_jsonl(output / 'graphs/graphs.jsonl'))
    expected = {p['file']:p for p in predictions if p['probability'] >= 0.5}
    findings = [x for x in report['findings'] if x['detector'] == 'model-gnn']
    assert len(findings) == len(expected)
    for finding in findings:
        assert finding['confidence'] == expected[finding['location']['file']]['probability']
    assert report['source_unchanged']
    write(output/'online-acceptance.json', {'functions':len(predictions),'model_findings':len(findings),
                                         'online_offline_probabilities_exact':True,'source_unchanged':True})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=['prepare','graphs','audit','plan','train','summarize','scan'])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=Path('artifacts/layer2/juliet-20260908-batch120'))
    parser.add_argument('--archive', type=Path, default=Path('artifacts/datasets/juliet-1.3.1/juliet-1.3.1.zip'))
    parser.add_argument('--compiler', type=Path, default=Path('D:/msys64/mingw64/bin/clang.exe'))
    parser.add_argument('--native', type=Path, default=Path('native/clang_tool/build/defectguard-clang.exe'))
    parser.add_argument('--encoder', type=Path, default=Path('artifacts/models/graphcodebert-base'))
    parser.add_argument('--run')
    args = parser.parse_args()
    globals()[args.phase](args)
