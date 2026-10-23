"""只读 Codex 轮次审批元数据，避免把自动审批误报成人工等待。"""
from __future__ import annotations

import json
import os
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class _ContextIndex:
    identity: tuple[int, int]
    offset: int = 0
    mtime_ns: int = 0
    reviewers: OrderedDict = field(default_factory=OrderedDict)


_INDEXES: OrderedDict[str, _ContextIndex] = OrderedDict()
_MAX_LINE = 1024 * 1024


def _reviewer_for_turn(path: str, turn_id: str) -> str | None:
    # 首次从文件头建立元数据索引，之后只读取新增内容。
    # 每次最多保留 1 MiB 的单行，缓存最多 32 个文件、每文件 128 个轮次。
    # 文件尾部可以增长任意长度，不会因此丢失早先的当前轮次设置。
    try:
        with open(path, "rb") as source:
            stat = os.fstat(source.fileno())
            identity = (stat.st_dev, stat.st_ino)
            index = _INDEXES.get(path)
            if (index is None or index.identity != identity or stat.st_size < index.offset or
                    (stat.st_size == index.offset and stat.st_mtime_ns != index.mtime_ns)):
                index = _ContextIndex(identity)
                _INDEXES[path] = index
            _INDEXES.move_to_end(path)
            while len(_INDEXES) > 32:
                _INDEXES.popitem(last=False)
            source.seek(index.offset)
            while True:
                start = source.tell()
                line = source.readline(_MAX_LINE + 1)
                if not line:
                    break
                if len(line) > _MAX_LINE:
                    # 大工具输出不需要解析；若超长行本身是上下文，则保守清空旧判断。
                    if b'"type":"turn_context"' in line[:256] or b'"type": "turn_context"' in line[:256]:
                        index.reviewers.clear()
                    while line and not line.endswith(b"\n"):
                        line = source.readline(_MAX_LINE + 1)
                    if not line:
                        source.seek(start)
                        break
                    index.offset = source.tell()
                    continue
                if not line.endswith(b"\n"):
                    break  # 保留半行起点，下次日志补齐后再读。
                index.offset = source.tell()
                if b'"turn_context"' not in line:
                    continue
                try:
                    row = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if not isinstance(row, dict) or row.get("type") != "turn_context":
                    continue
                payload = row.get("payload") or {}
                if not isinstance(payload, dict) or not isinstance(payload.get("turn_id"), str):
                    continue
                reviewer = payload.get("approvals_reviewer")
                tid = payload["turn_id"]
                index.reviewers[tid] = reviewer if isinstance(reviewer, str) else None
                index.reviewers.move_to_end(tid)
                while len(index.reviewers) > 128:
                    index.reviewers.popitem(last=False)
            index.mtime_ns = stat.st_mtime_ns
            return index.reviewers.get(turn_id)
    except OSError:
        return None


def read_approval_reviewer(payload: dict, *, sessions_dir: Path | None = None) -> str | None:
    """仅接受会话目录内的 transcript，且只匹配当前 turn_id。"""
    reviewer = payload.get("approvals_reviewer")
    if isinstance(reviewer, str):
        return reviewer
    transcript, turn_id = payload.get("transcript_path"), payload.get("turn_id")
    if not isinstance(transcript, str) or not isinstance(turn_id, str) or not turn_id:
        return None
    root = sessions_dir or (Path(os.environ.get("CODEX_HOME", "~/.codex")).expanduser() / "sessions")
    try:
        path = Path(transcript).resolve()
        if not path.is_relative_to(root.resolve()):
            return None
    except (OSError, ValueError):
        return None
    return _reviewer_for_turn(str(path), turn_id)
