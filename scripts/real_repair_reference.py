"""Typed AST reference comparison for the pinned benchmark, not a semantic proof."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


def command_arguments(command):
    if os.name != 'nt':
        return shlex.split(command)
    count = ctypes.c_int()
    shell = ctypes.windll.shell32
    shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    values = shell.CommandLineToArgvW(command, ctypes.byref(count))
    if not values:
        raise ValueError('cannot decode Windows compilation command')
    try:
        return [values[n] for n in range(count.value)]
    finally:
        ctypes.windll.kernel32.LocalFree(ctypes.cast(values, ctypes.c_void_p))


def canonical_ast(node, root):
    if not isinstance(node, dict):
        raise ValueError('expected Clang AST node')
    if node.get('kind') == 'ParenExpr':
        if len(node.get('inner', [])) != 1:
            raise ValueError('invalid parenthesis AST')
        return canonical_ast(node['inner'][0], root)
    fields = ('kind', 'name', 'opcode', 'castKind', 'valueCategory', 'isArrow', 'isPostfix',
              'hasElse', 'init', 'storageClass', 'isList', 'variadic', 'tagUsed')
    out = {key: node[key] for key in fields if key in node}
    if 'type' in node:
        out['type'] = {k: node['type'][k] for k in ('qualType', 'desugaredQualType') if k in node['type']}
    if 'value' in node:
        value = node['value']
        if node.get('kind') == 'StringLiteral':
            for prefix in sorted({str(root), root.as_posix(), str(root).replace('\\', '\\\\')}, key=len, reverse=True):
                value = value.replace(prefix, '$SOURCE')
        out['value'] = value
    if 'referencedDecl' in node:
        out['referencedDecl'] = canonical_ast(node['referencedDecl'], root)
    if 'inner' in node:
        out['inner'] = [canonical_ast(child, root) for child in node['inner']]
    return out


def reference_function(root, cfg, file, function, clang, output, env):
    entries = json.loads((cfg / 'compile_commands.json').read_text(encoding='utf-8'))
    entry = next(x for x in entries if Path(x['file']).resolve() == (root / file).resolve())
    arguments = list(entry.get('arguments') or command_arguments(entry['command']))
    cleaned = []
    n = 1
    while n < len(arguments):
        value = arguments[n]
        if value in ('-o', '--output'):
            n += 2
            continue
        if value == '-c':
            n += 1
            continue
        cleaned.append(value)
        n += 1
    command = [str(clang), *cleaned, '-fsyntax-only', '-Wno-unknown-warning-option',
               '-Xclang', '-ast-dump=json', '-Xclang', '-ast-dump-filter', '-Xclang', function]
    raw = output.with_suffix('.ast.json')
    with raw.open('wb') as stream:
        result = subprocess.run(command, cwd=entry['directory'], env=env, stdout=stream, stderr=subprocess.PIPE, timeout=120)
    output.with_suffix('.clang.log').write_bytes(result.stderr)
    output.with_suffix('.command.json').write_text(json.dumps(command, indent=2), encoding='utf-8')
    if result.returncode != 0 or raw.stat().st_size > 16 * 1024 * 1024:
        raise RuntimeError(f'Clang reference extraction failed: {output.name}')
    text = raw.read_text(encoding='utf-8')
    decoder, at, definitions = json.JSONDecoder(), 0, []
    while at < len(text):
        while at < len(text) and text[at].isspace():
            at += 1
        if at == len(text):
            break
        node, at = decoder.raw_decode(text, at)
        if node.get('kind') == 'FunctionDecl' and node.get('name') == function and any(c.get('kind') == 'CompoundStmt' for c in node.get('inner', [])):
            definitions.append(node)
    if len(definitions) != 1:
        raise ValueError(f'expected one definition of {function}, found {len(definitions)}')
    canonical = canonical_ast(definitions[0], root)
    encoded = json.dumps(canonical, sort_keys=True, separators=(',', ':')).encode('utf-8')
    output.with_suffix('.canonical.json').write_bytes(encoded)
    return dict(function=function, file=file, sha256=hashlib.sha256(encoded).hexdigest(),
                raw_ast_sha256=hashlib.sha256(raw.read_bytes()).hexdigest(), command=command)
