import json

from cchud_daemon.approval_context import read_approval_reviewer
from cchud_daemon.adapters.codex import CodexAdapter


def row(turn, reviewer):
    return json.dumps({"type": "turn_context", "payload": {
        "turn_id": turn, "approvals_reviewer": reviewer}}) + "\n"


def test_real_default_permission_with_automatic_reviewer(tmp_path):
    path = tmp_path / "rollout.jsonl"
    path.write_text(row("old", "user") + row("current", "auto_review"))
    # 真实 hook 仅给出 default，审批处理方需要从同一轮次元数据读取。
    payload = {"session_id": "session", "turn_id": "current",
               "transcript_path": str(path), "tool_name": "Bash",
               "permission_mode": "default"}
    adapter = CodexAdapter(reviewer_resolver=lambda p: read_approval_reviewer(p, sessions_dir=tmp_path))
    event = adapter.normalize({"event": "PermissionRequest", "payload": payload})[0]
    assert event.state == "thinking" and event.intervention_kind is None


def test_manual_approval_still_waits(tmp_path):
    path = tmp_path / "manual.jsonl"
    path.write_text(row("current", "user"))
    adapter = CodexAdapter(reviewer_resolver=lambda p: read_approval_reviewer(p, sessions_dir=tmp_path))
    event = adapter.normalize({"event": "PermissionRequest", "payload": {
        "turn_id": "current", "transcript_path": str(path), "permission_mode": "default"}})[0]
    assert event.state == "waiting" and event.intervention_kind == "approval"


def test_never_reuses_another_turn_or_reads_outside_sessions(tmp_path):
    root = tmp_path / "sessions"
    root.mkdir()
    path = root / "rollout.jsonl"
    path.write_text(row("old", "auto_review"))
    assert read_approval_reviewer({"turn_id": "new", "transcript_path": str(path)}, sessions_dir=root) is None
    outside = tmp_path / "outside.jsonl"
    outside.write_text(row("old", "auto_review"))
    assert read_approval_reviewer({"turn_id": "old", "transcript_path": str(outside)}, sessions_dir=root) is None


def test_reads_across_chunks_and_observes_newer_context(tmp_path):
    path = tmp_path / "rollout.jsonl"
    path.write_text(row("current", "auto_review") + json.dumps({"content": "x" * 100000}) + "\n")
    payload = {"turn_id": "current", "transcript_path": str(path)}
    assert read_approval_reviewer(payload, sessions_dir=tmp_path) == "auto_review"
    with path.open("a") as f:
        f.write(row("current", "user"))
    assert read_approval_reviewer(payload, sessions_dir=tmp_path) == "user"


def test_missing_metadata_is_not_assumed_automatic():
    assert read_approval_reviewer({"permission_mode": "default"}) is None


def test_context_remains_available_beyond_eight_megabytes(tmp_path):
    path=tmp_path/'long.jsonl'
    with path.open('w') as f:
        f.write(row('long-turn','auto_review'))
        f.write(json.dumps({'type':'response_item','payload':{'output':'x'*(9*1024*1024)}})+'\n')
    payload={'turn_id':'long-turn','transcript_path':str(path)}
    assert read_approval_reviewer(payload,sessions_dir=tmp_path)=='auto_review'
    with path.open('a') as f:f.write(row('long-turn','user'))
    assert read_approval_reviewer(payload,sessions_dir=tmp_path)=='user'


def test_partial_line_and_file_truncation_are_handled(tmp_path):
    path=tmp_path/'changing.jsonl'
    path.write_text(row('old','auto_review'))
    payload={'turn_id':'next','transcript_path':str(path)}
    assert read_approval_reviewer(payload,sessions_dir=tmp_path) is None
    line=row('next','user')
    with path.open('a') as f:f.write(line[:30])
    assert read_approval_reviewer(payload,sessions_dir=tmp_path) is None
    with path.open('a') as f:f.write(line[30:])
    assert read_approval_reviewer(payload,sessions_dir=tmp_path)=='user'
    path.write_text(row('replacement','auto_review'))
    assert read_approval_reviewer(payload,sessions_dir=tmp_path) is None
    assert read_approval_reviewer({'turn_id':'replacement','transcript_path':str(path)},sessions_dir=tmp_path)=='auto_review'


def test_unknown_reviewer_does_not_assert_human_waiting():
    for reviewer in (None,'unknown-future-reviewer'):
        adapter=CodexAdapter(reviewer_resolver=lambda p:reviewer)
        event=adapter.normalize({'event':'PermissionRequest','payload':{'session_id':'s','tool_name':'Bash'}})[0]
        assert event.state=='thinking' and event.phase=='review' and event.intervention_kind is None
