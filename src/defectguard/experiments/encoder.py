"""固定 GraphCodeBERT 权重的序列编码器；不下载/执行远程 Python 代码。"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import tempfile
import time

import torch
from safetensors import SafetensorError
from transformers import AutoModel, AutoTokenizer


MODEL_ID = "microsoft/graphcodebert-base"
MODEL_REVISION = "2b0488a7bb0eefc7041f1bb2cad1ab26b0da269d"
MODEL_FILES = ["config.json", "merges.txt", "pytorch_model.bin", "special_tokens_map.json",
               "tokenizer_config.json", "vocab.json", "README.md"]
POOLING_MODES = ("attention-masked-mean", "code-mean-max-v1")


def pool_hidden(hidden, attention_mask, pooling, special_mask=None):
    """Only retained code tokens contribute to the new mean/max representation."""
    mask = attention_mask.bool()
    if pooling == "code-mean-max-v1":
        if special_mask is None:
            raise ValueError("代码池化需要特殊符号掩码")
        mask = mask & ~special_mask.bool()
    elif pooling != "attention-masked-mean":
        raise ValueError("未知池化方式")
    if not mask.any(dim=1).all():
        raise ValueError("没有可池化的代码 token")
    mean = (hidden * mask.unsqueeze(-1)).sum(dim=1) / mask.sum(dim=1, keepdim=True)
    if pooling == "attention-masked-mean":
        return mean
    maximum = hidden.masked_fill(~mask.unsqueeze(-1), float('-inf')).amax(dim=1)
    return 0.5 * (mean + maximum)


def download_encoder(output: Path) -> Path:
    from huggingface_hub import snapshot_download
    return Path(snapshot_download(MODEL_ID, revision=MODEL_REVISION, local_dir=str(output),
                                  allow_patterns=MODEL_FILES))


def encoder_identity(directory: Path) -> str:
    """权重和 tokenizer 都参与指纹；缓存不能只依赖目录名称。"""
    if not directory.is_dir():
        raise ValueError(f"本地编码器目录不存在：{directory}；请先运行 download-encoder")
    digest = hashlib.sha256()
    files = sorted(path for path in directory.iterdir() if path.is_file() and path.suffix in {".bin", ".safetensors", ".json", ".txt"}
                   and path.name != "README.md")
    if not files or not any(path.suffix in {".bin", ".safetensors"} for path in files):
        raise ValueError("编码器目录缺少本地权重，请先运行 download-encoder")
    for path in files:
        digest.update(path.name.encode("utf-8"))
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest()


class FrozenEncoder:
    def __init__(self, directory: Path, max_length: int = 256, pooling: str = "attention-masked-mean"):
        if type(max_length) is not int or not 8 <= max_length <= 512:
            raise ValueError("序列长度必须位于 8 到 512")
        self.directory = directory.resolve()
        self.identity = encoder_identity(self.directory)
        self.max_length = max_length
        if pooling not in POOLING_MODES:
            raise ValueError("未知池化方式")
        self.pooling = pooling
        try:
            self.tokenizer = AutoTokenizer.from_pretrained(str(directory), local_files_only=True, trust_remote_code=False)
            self.model, loading = AutoModel.from_pretrained(
                str(directory), local_files_only=True, trust_remote_code=False,
                add_pooling_layer=False, output_loading_info=True)
        except (OSError, TypeError, RuntimeError, ValueError, SafetensorError) as error:
            raise ValueError(f"本地编码器加载失败：{self.directory}：{error}") from error
        # Transformers otherwise initializes missing backbone parameters randomly.
        # Extra pretrained MLM-head weights are harmless because AutoModel does not use that head.
        for field in ("missing_keys", "mismatched_keys", "error_msgs"):
            if loading.get(field):
                raise ValueError(f"编码器主干权重不完整或不兼容（{field}）：{loading[field]}")
        unsupported = [key for key in loading.get("unexpected_keys", []) if not key.startswith("lm_head.")]
        if unsupported:
            raise ValueError(f"编码器包含不支持的主干权重名称：{unsupported}")
        self.model.eval()
        self.model.requires_grad_(False)
        if self.model.config.model_type != "roberta" or self.model.config.hidden_size != 768:
            raise ValueError("当前序列基线需要 768 维 RoBERTa/GraphCodeBERT 编码器")
        self.dimension = self.model.config.hidden_size
        for name, parameter in self.model.named_parameters():
            if not parameter.is_floating_point() or not torch.isfinite(parameter).all():
                raise ValueError(f"编码器权重包含非有限值或不支持的类型：{name}")
        if len(self.tokenizer) != self.model.get_input_embeddings().num_embeddings:
            raise ValueError("编码器 tokenizer 词表大小与词嵌入不匹配")

    def encode(self, records: list[dict], cache: Path | None = None) -> tuple[list[torch.Tensor], dict]:
        from safetensors import safe_open
        from safetensors.torch import save_file
        vectors, details = [], []
        pooling = getattr(self, 'pooling', 'attention-masked-mean')
        cache_version = 'masked-mean-v1' if pooling == 'attention-masked-mean' else pooling
        if cache:
            cache.mkdir(parents=True, exist_ok=True)
        for record in records:
            text = record["source"]
            key = hashlib.sha256(json.dumps([self.identity, self.max_length, cache_version, text],
                                             ensure_ascii=False).encode("utf-8")).hexdigest()
            cache_file = cache / f"{key}.safetensors" if cache else None
            token_count = len(self.tokenizer(text, truncation=False, verbose=False)["input_ids"])
            cache_status = "disabled" if cache_file is None else "miss"
            if cache_file and cache_file.is_file():
                try:
                    with safe_open(str(cache_file), framework="pt", device="cpu") as stored:
                        if list(stored.keys()) != ["embedding"]:
                            raise ValueError("需要唯一 embedding 张量")
                        vector = stored.get_tensor("embedding")
                        metadata = stored.metadata()
                    if metadata is not None and (
                        metadata.get("cache_key") != key or
                        metadata.get("embedding_sha256") != hashlib.sha256(vector.numpy().tobytes()).hexdigest()
                    ):
                        raise ValueError("缓存指纹或内容哈希不一致")
                    cache_status = "hit" if metadata is not None else "legacy-shape-checked"
                except (OSError, RuntimeError, ValueError, TypeError, SafetensorError) as error:
                    raise ValueError(f"序列特征缓存损坏：{cache_file}；移走该文件后可重新计算。原因：{error}") from error
            else:
                options = {'return_special_tokens_mask': True} if pooling != 'attention-masked-mean' else {}
                tokens = self.tokenizer(text, truncation=True, max_length=self.max_length, return_tensors="pt", **options)
                special_mask = tokens.pop('special_tokens_mask', None)
                with torch.inference_mode():
                    hidden = self.model(**tokens).last_hidden_state
                    vector = pool_hidden(hidden, tokens['attention_mask'], pooling, special_mask).squeeze(0).contiguous()
            if vector.shape != (self.dimension,) or vector.dtype != torch.float32 or not torch.isfinite(vector).all():
                raise ValueError(f"序列特征形状、类型或数值无效：{cache_file or record.get('sample_id', record.get('graph_id'))}")
            if cache_file and cache_status == "miss":
                descriptor, temporary = tempfile.mkstemp(prefix=key + ".", suffix=".tmp", dir=cache)
                os.close(descriptor)
                try:
                    save_file({"embedding": vector}, temporary, metadata={
                        "cache_key": key, "embedding_sha256": hashlib.sha256(vector.numpy().tobytes()).hexdigest()})
                    # Antivirus/indexing can briefly hold a Windows file handle.
                    # Bounded retries preserve atomic replacement, never a partial cache.
                    for attempt in range(5):
                        try:
                            os.replace(temporary, cache_file)
                            break
                        except PermissionError:
                            if attempt == 4:
                                raise
                            time.sleep(0.05 * (attempt + 1))
                except OSError as error:
                    raise ValueError(f"序列特征缓存写入失败：{cache_file}：{error}") from error
                finally:
                    Path(temporary).unlink(missing_ok=True)
            # Detach from inference tensors before passing through trainable layers.
            vectors.append(vector.clone().detach())
            details.append({"sample_id": record.get("sample_id", record.get("graph_id")),
                            "file": record.get("file"), "function": record.get("function"),
                            "start_line": record.get("start_line"), "end_line": record.get("end_line"),
                            "token_count": token_count, "retained_token_count": min(token_count, self.max_length),
                            "truncated": token_count > self.max_length, "cache_status": cache_status})
        return vectors, {"encoder_sha256": self.identity, "max_length": self.max_length,
                         "pooling": pooling, "frozen": True,
                         "usage": "sequence-only; original graph-guided attention is not reproduced",
                         "truncated_functions": sum(item["truncated"] for item in details), "functions": details}
