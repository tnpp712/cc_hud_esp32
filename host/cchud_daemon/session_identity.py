"""持久化会话短编号，仅保存会话 ID 和编号，不保存对话正文。"""
import hashlib
import json
from pathlib import Path


class SessionIdentity:
    def __init__(self, path: Path | None = None):
        self.path = path
        self.codes = {}
        if path:
            try:
                values = json.loads(path.read_text())
                if isinstance(values, dict):
                    self.codes = {sid: code for sid, code in values.items()
                                  if isinstance(code, str) and 4 <= len(code) <= 12
                                  and all(c in '0123456789ABCDEF' for c in code)}
            except (OSError, ValueError):
                pass

    def code(self, sid):
        if sid in self.codes:
            return self.codes[sid]
        used = set(self.codes.values())
        digest = hashlib.sha256(sid.encode()).hexdigest().upper()
        for size in range(4, 13, 2):
            candidate = digest[:size]
            if candidate not in used:
                break
        else:
            # 极少见的长前缀碰撞仍分配独立编号，分配结果落盘后保持固定。
            salt = 0
            while candidate in used:
                salt += 1
                candidate = hashlib.sha256(f'{sid}:{salt}'.encode()).hexdigest()[:12].upper()
        self.codes[sid] = candidate
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temp = self.path.with_suffix('.tmp')
            temp.write_text(json.dumps(self.codes, ensure_ascii=False))
            temp.replace(self.path)
        return candidate
