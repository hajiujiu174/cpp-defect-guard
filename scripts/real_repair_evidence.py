"""Capture exact proposal bytes and verify a downloaded evidence directory."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_record(group, label, manifest):
    if group not in ('proposals', 'sets'):
        raise ValueError('Unknown evidence group')
    manifest = Path(manifest)
    expected = '.cgfix' if group == 'proposals' else '.cgset'
    if manifest.suffix.lower() != expected:
        raise ValueError('Unexpected manifest extension')
    files = []
    for path in (manifest, manifest.with_name('candidate.diff')):
        if path.is_symlink() or not path.is_file():
            raise ValueError('Evidence source missing or symlink: '+str(path))
        data = path.read_bytes()
        if not data:
            raise ValueError('Empty evidence source: '+str(path))
        files.append({'source':path, 'bytes':len(data), 'sha256':digest(data)})
    return {'group':group, 'label':label, 'files':files}


def capture_evidence(destination, sources):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    records = []
    for index, source in enumerate(sources):
        for entry in source['files']:
            data = entry['source'].read_bytes()
            if len(data) != entry['bytes'] or digest(data) != entry['sha256']:
                raise ValueError('Evidence changed after generation: '+str(entry['source']))
            relative = PurePosixPath(source['group'], str(index), entry['source'].name)
            target = destination.joinpath(*relative.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as stream:
                stream.write(data)
            records.append({'group':source['group'], 'label':source['label'], 'path':relative.as_posix(),
                            'bytes':len(data), 'sha256':entry['sha256']})
    manifest = {'format':'CodeGuardRepairEvidence 1', 'files':records}
    with (destination/'manifest.json').open('x', encoding='utf-8') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    return verify_evidence(destination)


def verify_evidence(directory):
    directory = Path(directory).resolve(strict=True)
    manifest = json.loads((directory/'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('format') != 'CodeGuardRepairEvidence 1' or not manifest.get('files'):
        raise ValueError('Invalid evidence manifest')
    expected = {'manifest.json'}
    groups = {'proposals':set(), 'sets':set()}
    for record in manifest['files']:
        name = record['path']
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name or name in expected:
            raise ValueError('Unsafe or duplicate evidence path')
        if record['group'] not in groups or not path.parts or path.parts[0] != record['group']:
            raise ValueError('Evidence group does not match its path')
        target = directory.joinpath(*path.parts)
        if target.is_symlink() or not target.resolve(strict=True).is_relative_to(directory):
            raise ValueError('Evidence path escapes directory')
        data = target.read_bytes()
        if len(data) != record['bytes'] or digest(data) != record['sha256']:
            raise ValueError('Evidence byte checksum mismatch: '+name)
        expected.add(name)
        if target.suffix.lower() in ('.cgfix','.cgset'):
            groups[record['group']].add(record['label'])
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    if actual != expected or not all(groups.values()):
        raise ValueError('Evidence inventory incomplete or contains unlisted files')
    return {'proposals':len(groups['proposals']), 'sets':len(groups['sets']),
            'files':len(manifest['files']), 'manifest_sha256':digest((directory/'manifest.json').read_bytes())}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(verify_evidence(args.verify), ensure_ascii=False))
