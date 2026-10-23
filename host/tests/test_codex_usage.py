import json
from datetime import datetime, timezone

from cchud_daemon.codex_usage import CodexUsageReader, usage_event, changed_lines


def test_weekly_primary_is_not_five_hour_quota():
    event = usage_event({"rate_limits": {"limit_id": "codex", "primary": {
        "window_minutes": 10080, "used_percent": 12, "resets_at": 2000}}}, "s")
    assert event.five_h_used is None
    assert event.seven_d_used == 12
    assert event.metrics_valid == 2
    assert event.cost_usd is None and event.lines_added is None


def test_context_uses_last_request_not_cumulative_tokens():
    event = usage_event({"info": {"model_context_window": 1000,
        "last_token_usage": {"total_tokens": 250},
        "total_token_usage": {"total_tokens": 9000}}}, "s", 17)
    assert event.ctx_pct == 25 and event.metrics_valid == 20


def test_unknown_window_and_other_limit_are_not_mislabeled():
    for limit in ({"primary": {"window_minutes": 60, "used_percent": 50}},
                  {"limit_id": "other", "primary": {"window_minutes": 300, "used_percent": 50}}):
        assert usage_event({"rate_limits": limit}, "s").metrics_valid == 0


def test_reader_partial_line_and_incremental_offset(tmp_path):
    now = datetime.now(timezone.utc)
    folder = tmp_path / "sessions" / now.astimezone().strftime("%Y/%m/%d")
    folder.mkdir(parents=True)
    path = folder / "rollout.jsonl"
    row = {"timestamp": now.isoformat(), "type": "event_msg", "payload": {
        "type": "token_count", "rate_limits": {"primary": {
            "window_minutes": 10080, "used_percent": 0}}}}
    encoded = json.dumps(row)
    path.write_text(encoded[:50])
    reader = CodexUsageReader(tmp_path, now=lambda: now.timestamp())
    assert reader.poll() == []
    with path.open("a") as f: f.write(encoded[50:] + "\n")
    events = reader.poll()
    assert len(events) == 1 and events[0].seven_d_used == 0
    assert events[0].metrics_valid == 2
    assert reader.poll() == []


def test_reader_ignores_stale_metrics(tmp_path):
    now = datetime.now(timezone.utc)
    folder = tmp_path / "sessions" / now.astimezone().strftime("%Y/%m/%d")
    folder.mkdir(parents=True)
    (folder / "old.jsonl").write_text(json.dumps({"timestamp": "2000-01-01T00:00:00Z",
        "type": "event_msg", "payload": {"type": "token_count"}}) + "\n")
    assert CodexUsageReader(tmp_path).poll() == []


def test_successful_file_change_counts_exclude_diff_headers():
    assert changed_lines({"a": {"type": "update", "unified_diff":
        "--- a\n+++ b\n@@ -1 +1 @@\n-old\n+new\n+extra\n"},
        "b": {"type": "add", "content": "one\ntwo\n"}}) == (4, 1)
    assert changed_lines({"a": {"type": "unknown"}}) is None


def test_five_hour_window_is_ignored_even_when_reported():
    event = usage_event({"rate_limits": {"primary": {
        "window_minutes": 300, "used_percent": 99}}}, "s")
    assert event.five_h_used is None and event.metrics_valid == 0


def test_metadata_and_internal_review_filter(tmp_path):
    now=datetime.now(timezone.utc)
    folder=tmp_path/'sessions'/now.astimezone().strftime('%Y/%m/%d')
    folder.mkdir(parents=True)
    def save(name,model):
        rows=[{'type':'session_meta','payload':{'id':name}},
              {'type':'event_msg','payload':{'type':'task_started','turn_id':'t'}},
              {'type':'turn_context','payload':{'turn_id':'t','cwd':'/repo/project','model':model,'effort':'high'}},
              {'type':'event_msg','payload':{'type':'token_count','rate_limits':{'primary':{'window_minutes':10080,'used_percent':12}}}}]
        (folder/(name+'.jsonl')).write_text(''.join(json.dumps({**r,'timestamp':now.isoformat()})+'\n' for r in rows))
    save('review','codex-auto-review');save('work','model')
    reader=CodexUsageReader(tmp_path,now=lambda:now.timestamp())
    events=reader.poll()
    assert {e.session_id for e in events}=={'work'}
    meta=next(e for e in events if e.kind=='metadata')
    assert meta.project=='project' and meta.effort=='high' and meta.started_at==now.timestamp()
    assert reader.poll()==[]


def test_active_older_thread_is_discovered_and_context_is_per_session(tmp_path):
    import sqlite3
    now=datetime.now(timezone.utc)
    folder=tmp_path/'sessions'/'2026'/'01'/'01';folder.mkdir(parents=True)
    path=folder/'old-created-session.jsonl'
    rows=[{'type':'session_meta','payload':{'id':'old'}},
          {'type':'turn_context','payload':{'turn_id':'t','cwd':'/repo/project','model':'model','effort':'high'}},
          {'type':'event_msg','payload':{'type':'token_count','info':{'model_context_window':1000,'last_token_usage':{'total_tokens':400}}}}]
    path.write_text(''.join(json.dumps({**r,'timestamp':now.isoformat()})+'\n' for r in rows))
    c=sqlite3.connect(tmp_path/'state_5.sqlite')
    c.execute('create table threads (rollout_path text, archived integer, updated_at integer)')
    c.execute('insert into threads values (?,0,?)',(str(path),int(now.timestamp())));c.commit();c.close()
    reader=CodexUsageReader(tmp_path,now=lambda:now.timestamp())
    meta=next(e for e in reader.poll() if e.kind=='metadata')
    assert meta.effort=='high' and meta.ctx_pct==40
    assert meta.pending_questions==0


def test_hook_registered_source_is_restored_after_restart(tmp_path):
    root=tmp_path/'sessions'/'old';root.mkdir(parents=True)
    path=root/'file.jsonl';path.write_text('')
    state=tmp_path/'sources.json'
    reader=CodexUsageReader(tmp_path,sources_path=state);reader.track_path(str(path))
    restored=CodexUsageReader(tmp_path,sources_path=state)
    assert path in restored.tracked
    restored.untrack_path(str(path))
    assert path not in CodexUsageReader(tmp_path,sources_path=state).tracked
