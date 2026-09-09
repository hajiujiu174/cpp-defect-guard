"""Import the pinned NIST Juliet archive without executing its build scripts or tests.

The initial profile accepts C, local control-flow variants, void/no-argument leaves.
Labels come from Juliet's bad/good construction, never from our static rules.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import zipfile

from defectguard.analysis.source_utils import mask_comments_and_strings
from .data import canonical_hash


ARCHIVE_URL = "https://samate.nist.gov/SARD/downloads/test-suites/2022-08-11-juliet-c-cplusplus-v1-3-1-with-extra-support.zip"
ARCHIVE_SHA256 = "331b288f17ea95076d76e7bcc1d0e307f9e78241b16e744340213c8e2986919a"
DEFAULT_CWES = (121, 122, 124, 126, 127, 369, 401, 415, 416, 457, 476)
DEFAULT_FLOWS = (1, 2, 3, 4, 5, 6, 7, 15, 16, 17, 18)
PROFILE = "c-local-flow-leaf-pairs-v2"
_FUNCTION = re.compile(r"(?m)^[ \t]*(?:static\s+)?void\s+(?P<name>[A-Za-z_]\w*)\s*\(\s*(?:void)?\s*\)\s*\{")
_TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d+(?:\.\d*)?[A-Za-z0-9_]*|>>=|<<=|->|\+\+|--|<=|>=|==|!=|&&|\|\||[^\s]')
_LEAK = re.compile(r"CWE\d+|good|bad|POTENTIAL\s+FLAW|\bFIX\s*:", re.I)
_KEYWORDS = set("auto break case char const continue default do double else enum extern float for goto if int long register return short signed sizeof static struct switch typedef union unsigned void volatile while _Bool _Complex _Imaginary".split())


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _hash(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")


def _write_jsonl(path: Path, records: list[dict]) -> None:
    path.write_text("".join(json.dumps(record, ensure_ascii=False, sort_keys=True, allow_nan=False) + "\n" for record in records), encoding="utf-8", newline="\n")


def _member_text(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    if info.file_size > 2 * 1024 * 1024 or info.flag_bits & 1:
        raise ValueError("archive member too large or encrypted")
    return archive.read(info).decode("utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")


def _validate_members(infos: list[zipfile.ZipInfo]) -> None:
    seen = set()
    for info in infos:
        path = PurePosixPath(info.filename)
        if (path.is_absolute() or ".." in path.parts or "\\" in info.filename or ":" in info.filename
                or any(part.rstrip(" .") != part for part in path.parts)
                or stat.S_ISLNK(info.external_attr >> 16)):
            raise ValueError(f"不安全的 ZIP 路径或符号链接：{info.filename}")
        key = path.as_posix().casefold()
        if key in seen:
            raise ValueError(f"ZIP 包含重复或大小写冲突路径：{info.filename}")
        seen.add(key)


def strip_comments(text: str) -> str:
    """Preserve strings/chars and line mapping; reject unsupported phase-2 splices."""
    if "\\\n" in text or "\\\r" in text:
        raise ValueError("line-spliced source outside initial profile")
    return _TOKEN.sub(lambda match: "".join("\n" if c == "\n" else " " for c in match[0])
                      if match[0].startswith(("//", "/*")) else match[0], text)


def template_hash(source: str) -> str:
    """Conservative clone grouping only, NEVER used to merge labels or rewrite code.

    Fold identifiers and literals but retain types, operators, control flow, and
    called APIs. Thus many type variants are already grouped via Label Definition.
    """
    tokens = [m[0] for m in _TOKEN.finditer(strip_comments(source))]
    identifiers, abstract = {}, []
    for index, token in enumerate(tokens):
        if token.startswith(('"', "'")) or token[0].isdigit():
            abstract.append("<literal>")
        elif re.fullmatch(r"[A-Za-z_]\w*", token) and token not in _KEYWORDS:
            if index + 1 < len(tokens) and tokens[index + 1] == "(":
                abstract.append(token)  # preserve API calls
            else:
                abstract.append(identifiers.setdefault(token, f"v{len(identifiers)}"))
        else:
            abstract.append(token)
    return _hash(" ".join(abstract))


def clean_local_names(code: str) -> tuple[str, dict[str, str]]:
    """Injective spelling substitution for a bounded set of local C declarations.

    No strings, members, callees, macros or external names are rewritten. Shadowed
    declarations keep their scopes because the same spelling maps bijectively.
    Unsupported names are rejected by the caller, never guessed.
    """
    masked = mask_comments_and_strings(code)
    identifiers = set(re.findall(r"\b[A-Za-z_]\w*\b", masked))
    mapping = {}
    declarations = re.finditer(
        r"\b(?:const\s+)?(?:unsigned\s+|signed\s+)?(?:char|wchar_t|int|short|long|float|double)\s+\**\s*([A-Za-z_]\w*)\s*(?=[=;\[])", masked)
    for match in declarations:
        name = match[1]
        if not _LEAK.search(name) or name in mapping:
            continue
        # Reject names also used as member, callable, label or preprocessor token.
        if re.search(r"(?:\.|->)\s*" + re.escape(name) + r"\b|\b" + re.escape(name) + r"\s*[:(]", masked) or "#" in masked:
            raise ValueError("answer-bearing name outside local variable profile")
        neutral = f"local_slot_{len(mapping)}"
        while neutral in identifiers:
            neutral += "_"
        identifiers.add(neutral)
        mapping[name] = neutral
    return _TOKEN.sub(lambda m: mapping.get(m[0], m[0]), code), mapping


def extract_pair(text: str, filename: str, seed: int, *, flows: tuple[int, ...] = DEFAULT_FLOWS) -> tuple[list[dict], str]:
    flow = re.search(r"_(\d\d)\.c$", filename)
    if not flow or int(flow[1]) not in flows or not re.search(r"Flow Variant:\s*" + flow[1] + r"\b", text):
        raise ValueError("missing or unsupported local-flow metadata")
    definition = re.search(r"Label Definition File:\s*([^\r\n]+)\.label\.xml", text)
    if not definition or not re.fullmatch(r"[A-Za-z0-9_.-]+", definition[1]):
        raise ValueError("missing or unsupported label-definition metadata")
    clean = strip_comments(text)
    masked = mask_comments_and_strings(clean)
    functions = []
    for match in _FUNCTION.finditer(masked):
        brace = masked.index("{", match.start(), match.end())
        depth, end = 1, brace + 1
        while end < len(masked) and depth:
            depth += (masked[end] == "{") - (masked[end] == "}")
            end += 1
        if depth:
            raise ValueError("unbalanced function body")
        functions.append({"name": match["name"], "start": match.start(), "brace": brace, "end": end})
    names = {fn["name"] for fn in functions}
    bad_name = PurePosixPath(filename).stem + "_bad"
    bad = [fn for fn in functions if fn["name"] == bad_name]
    good = [fn for fn in functions if re.fullmatch(r"good(?:G2B|B2G)?\d*", fn["name"])]
    # Wrapper functions are not negative training examples: they merely call good leaves.
    def leaf(fn):
        body = masked[fn["brace"]:fn["end"]]
        return not any(re.search(r"\b" + re.escape(name) + r"\s*\(", body) for name in names)
    if len(bad) != 1 or not leaf(bad[0]):
        raise ValueError("missing unique self-contained bad function")
    good = [fn for fn in good if leaf(fn)]
    if not good:
        raise ValueError("no self-contained good leaf function")
    first_function = min(fn["start"] for fn in functions)
    boundary = re.search(r"(?m)^\s*#\s*ifndef\s+OMITBAD\b", clean[:first_function])
    if not boundary:
        raise ValueError("unsupported testcase preamble")
    prefix = clean[:boundary.start()]
    if re.search(r"(?m)^\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b", prefix) or "{" in mask_comments_and_strings(prefix):
        raise ValueError("conditional or function-bearing preamble outside profile")
    negative = min(good, key=lambda fn: _hash(f"{seed}:{filename}:{fn['name']}"))
    pair = []
    for fn, label in ((bad[0], 1), (negative, 0)):
        code = clean[fn["start"]:fn["end"]]
        code = re.sub(r"^(?:static\s+)?void\s+" + re.escape(fn["name"]) + r"\s*\(\s*(?:void)?\s*\)",
                      "void process(void)", code.lstrip(), count=1)
        code, renamed = clean_local_names(code)
        # Keep source line mapping even after removing comments and empty lines.
        prefix_lines = [(line.rstrip(), i + 1) for i, line in enumerate(prefix.splitlines()) if line.strip()]
        original_line = text.count("\n", 0, fn["start"]) + 1
        body_lines = [(line.rstrip(), original_line + i) for i, line in enumerate(code.splitlines()) if line.strip()]
        emitted = "\n".join(line for line, _ in prefix_lines + body_lines) + "\n"
        if _LEAK.search(emitted):
            raise ValueError("answer-bearing tokens remain in isolated source")
        if re.search(r'(?m)^\s*#\s*include\s*"(?!std_testcase(?:_io)?\.h")', emitted):
            raise ValueError("custom quoted include outside profile")
        if re.search(r"(?m)^\s*#", "\n".join(line for line, _ in body_lines)):
            raise ValueError("preprocessor inside selected function outside profile")
        source = "\n".join(line for line, _ in body_lines)
        pair.append({"label": label, "original_function": fn["name"], "code": emitted,
                     "renamed_identifiers": renamed,
                     "original_function_line": original_line,
                     "line_map": [line for _, line in prefix_lines + body_lines],
                     "function_source": source, "canonical_sha256": canonical_hash(source),
                     "template_sha256": template_hash(source)})
    if pair[0]["canonical_sha256"] == pair[1]["canonical_sha256"]:
        raise ValueError("identical positive/negative function source")
    return pair, definition[1]


def import_juliet(archive_path: Path, output: Path, *, max_cases: int = 1500,
                  cwes: tuple[int, ...] = DEFAULT_CWES, seed: int = 42,
                  flows: tuple[int, ...] = DEFAULT_FLOWS,
                  expected_sha256: str = ARCHIVE_SHA256, selection: Path | None = None) -> dict:
    if type(max_cases) is not int or not 3 <= max_cases <= 20000 or type(seed) is not int:
        raise ValueError("max_cases 必须为 3..20000，seed 必须为整数")
    if not cwes or any(type(cwe) is not int or cwe not in DEFAULT_CWES for cwe in cwes) or len(set(cwes)) != len(cwes):
        raise ValueError(f"首批支持的 CWE 为 {DEFAULT_CWES}，不得重复")
    if not flows or any(type(flow) is not int or flow not in DEFAULT_FLOWS for flow in flows) or len(set(flows)) != len(flows):
        raise ValueError(f"首批支持的控制流变体为 {DEFAULT_FLOWS}，不得重复")
    archive_path, output = archive_path.resolve(), output.resolve()
    if archive_path.is_relative_to(output) or output.is_relative_to(archive_path.parent):
        raise ValueError("导入输出必须与下载归档目录相互独立")
    if output.exists():
        raise ValueError("输出目录已存在，拒绝覆盖既有语料或实验")
    identity = sha256_file(archive_path)
    if identity != expected_sha256:
        raise ValueError("Juliet 归档 SHA-256 与指定版本不一致，不导入未核验数据")
    selected, rejected, originals, support, licenses = [], [], {}, {}, {}
    selected_paths = None
    if selection is not None:
        from .data import read_jsonl
        selected_paths = {item["source_file"] for item in read_jsonl(selection)}
        if len(selected_paths) != max_cases:
            raise ValueError("固定选样清单的用例数必须等于 max_cases")
    with zipfile.ZipFile(archive_path) as archive:
        infos = archive.infolist()
        _validate_members(infos)
        candidates, support_infos = defaultdict(list), defaultdict(list)
        for info in infos:
            parts = PurePosixPath(info.filename).parts
            name = PurePosixPath(info.filename).name
            if "testcasesupport" in parts and name.endswith(".h"):
                support_infos[parts[:parts.index("testcasesupport")]].append(info)
            if re.fullmatch(r"(?:LICENSE|COPYING|NOTICE|README)(?:\.[^/]*)?", name, re.I) and len(parts) <= 3:
                licenses[info.filename] = _member_text(archive, info)
            match = re.match(r"CWE(\d+)_.*_(\d\d)\.c$", name)
            if "testcases" in parts and match and int(match[1]) in cwes and int(match[2]) in flows and (selected_paths is None or info.filename in selected_paths):
                candidates[int(match[1])].append(info)
        if not support_infos or not candidates:
            raise ValueError("未找到 Juliet 测试支持头文件或首批候选")
        ordered = []
        for cwe in sorted(candidates):
            candidates[cwe].sort(key=lambda info: _hash(f"{seed}:{info.filename}"))
        # Round robin by CWE avoids filling the batch with only the largest category.
        for position in range(max(map(len, candidates.values()))):
            ordered.extend((cwe, candidates[cwe][position]) for cwe in sorted(candidates) if position < len(candidates[cwe]))
        for cwe, info in ordered:
            if len(originals) == max_cases:
                break
            try:
                original = _member_text(archive, info)
                pair, definition = extract_pair(original, info.filename, seed, flows=flows)
                parts = PurePosixPath(info.filename).parts
                local_support = {PurePosixPath(item.filename).name: _member_text(archive, item)
                                 for item in support_infos[parts[:parts.index("testcases")]]}
                if "std_testcase.h" not in local_support:
                    raise ValueError("missing testcase-specific support headers")
                if any(name in support and support[name] != value for name, value in local_support.items()):
                    raise ValueError("conflicting testcase support headers; cannot merge compile contexts")
            except (ValueError, UnicodeDecodeError) as error:
                rejected.append({"source_file": info.filename, "reason": str(error)})
                continue
            originals[info.filename] = archive.read(info)
            support.update(local_support)
            for fn in pair:
                fn.update(source_file=info.filename, cwe=cwe, family=definition,
                          original_sha256=hashlib.sha256(originals[info.filename]).hexdigest())
                selected.append(fn)
    if selected_paths is not None and set(originals) != selected_paths:
        raise ValueError("固定选样清单包含缺失或无法清洗的用例；不替换对照样例")
    if len(originals) < 3:
        raise ValueError("可接受的修复对不足三个，未创建输出")
    # Group complete Juliet template families and additional normalized near clones.
    parents = {item["family"]: item["family"] for item in selected}
    def find(key):
        while parents[key] != key:
            parents[key] = parents[parents[key]]
            key = parents[key]
        return key
    hashes = {}
    for item in selected:
        for key in ("exact:" + item["canonical_sha256"], "clone:" + item["template_sha256"]):
            if key in hashes:
                left, right = find(item["family"]), find(hashes[key])
                parents[max(left, right)] = min(left, right)
            hashes[key] = item["family"]
    if len({find(key) for key in parents}) < 3:
        raise ValueError("合并模板和近克隆后不足三个隔离组；请扩大 CWE 范围")
    corpus = output / "corpus"
    (corpus / "functions").mkdir(parents=True)
    (corpus / "support").mkdir()
    (output / "originals").mkdir()
    (output / "licenses").mkdir()
    for name, content in support.items():
        (corpus / "support" / name).write_text(content, encoding="utf-8", newline="\n")
    for name, content in licenses.items():
        (output / "licenses" / (_hash(name)[:12] + "-" + PurePosixPath(name).name)).write_text(content, encoding="utf-8", newline="\n")
    labels, provenance = [], []
    for name, content in originals.items():
        (output / "originals" / (_hash(name) + ".c")).write_bytes(content)
    for item in sorted(selected, key=lambda item: _hash(item["source_file"] + ":" + item["original_function"])):
        sample_id = _hash(item["source_file"] + ":" + item["original_function"])
        relative = f"functions/{sample_id[:24]}.c"
        target = corpus / relative
        if target.exists():
            raise ValueError("导入样例路径哈希冲突")
        target.write_text(item["code"], encoding="utf-8", newline="\n")
        labels.append({"sample_id": "juliet-" + sample_id, "file": relative, "function": "process",
                       "label": item["label"], "group_id": "juliet-family-" + _hash(find(item["family"]))[:24],
                       "label_source": "nist-juliet-1.3.1-good-bad-convention",
                       "defect_lines": [],
                       "provenance": {"dataset": "Juliet C/C++ 1.3.1", "archive_sha256": identity,
                                      "source_file": item["source_file"], "original_function": item["original_function"],
                                      "cwe": item["cwe"], "template_family": item["family"]},
                       "rationale": "Juliet controlled bad function" if item["label"] else "Juliet controlled good leaf; not a general safety proof"})
        provenance.append({"sample_id": "juliet-" + sample_id, "file": relative,
                           "renamed_identifiers": item["renamed_identifiers"],
                           "source_file": item["source_file"], "original_sha256": item["original_sha256"],
                           "original_function": item["original_function"], "original_function_line": item["original_function_line"],
                           "generated_file_sha256": _hash(item["code"]), "line_map": item["line_map"],
                           "template_sha256": item["template_sha256"], "template_family": item["family"]})
    _write_jsonl(output / "labels.jsonl", labels)
    _write_jsonl(output / "provenance.jsonl", provenance)
    (corpus / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(juliet_import LANGUAGES C)\nset(CMAKE_C_STANDARD 11)\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON)\nfile(GLOB SOURCES CONFIGURE_DEPENDS "functions/*.c")\nadd_library(juliet_cases OBJECT ${SOURCES})\ntarget_include_directories(juliet_cases PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")\n', encoding="utf-8", newline="\n")
    manifest = {"schema_version": "1.0", "status": "imported-not-yet-compiled", "profile": PROFILE,
                "source_url": ARCHIVE_URL, "archive_sha256": identity,
                "official_archive_verified": identity == ARCHIVE_SHA256,
                "seed": seed, "requested_max_cases": max_cases, "requested_cwes": list(cwes),
                "requested_flows": list(flows),
                "candidate_cases": sum(map(len, candidates.values())), "selected_cases": len(originals),
                "imported_functions": len(labels), "class_counts": dict(Counter(str(label["label"]) for label in labels)),
                "cwe_case_counts": dict(Counter(str(item["cwe"]) for item in selected if item["label"] == 1)),
                "template_family_count": len(parents), "clone_merged_group_count": len({find(key) for key in parents}),
                "rejected_cases": rejected, "unexamined_cases": len(ordered) - len(originals) - len(rejected),
                "labels_sha256": sha256_file(output / "labels.jsonl"), "provenance_sha256": sha256_file(output / "provenance.jsonl"),
                "license_documents": sorted(licenses),
                "limitations": ["C local-control-flow leaf pairs only; C++ and interprocedural cases excluded",
                                "Synthetic, possible residual template cues; not a real-project benchmark",
                                "No trusted defect-line labels; classification evaluation only",
                                "Normalized-clone grouping is a conservative heuristic, not semantic clone proof",
                                "Source is compile-only; vulnerable functions must not be executed"]}
    _write_json(output / "import-manifest.json", manifest)
    return {key: value for key, value in manifest.items() if key != "rejected_cases"} | {"rejected_case_count": len(rejected), "output": str(output)}
