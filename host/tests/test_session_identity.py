import hashlib
import json
import sqlite3
from datetime import datetime, timezone

from cchud_daemon.session_identity import SessionIdentity
from cchud_daemon.codex_usage import CodexUsageReader
from cchud_daemon.session_store import SessionStore
from cchud_daemon.event import CcHudEvent
from cchud_daemon.codec import encode_work_frames, decode_work_readback


def test_short_id_collision_restart_and_order(tmp_path):
    seen = {}
    for n in range(10000):
        sid = str(n)
        prefix = hashlib.sha256(sid.encode()).hexdigest()[:4]
        if prefix in seen:
            first, second = seen[prefix], sid
            break
        seen[prefix] = sid
    ids = SessionIdentity(tmp_path/'ids.json')
    a, b = ids.code(first), ids.code(second)
    assert len(a)==4 and len(b)==6 and a!=b
    restored = SessionIdentity(tmp_path/'ids.json')
    assert restored.code(second)==b and restored.code(first)==a


def test_title_rename_without_log_change_and_stable_number(tmp_path):
    now = datetime.now(timezone.utc)
    clock = [now.timestamp()]
    folder = tmp_path/'sessions'/'old';folder.mkdir(parents=True)
    path = folder/'thread.jsonl'
    rows=[{'type':'session_meta','payload':{'id':'s'}},
          {'type':'turn_context','payload':{'turn_id':'t','cwd':'/repo/project','model':'model'}}]
    path.write_text(''.join(json.dumps({**r,'timestamp':now.isoformat()})+'\n' for r in rows))
    c=sqlite3.connect(tmp_path/'state_5.sqlite')
    c.execute('create table threads (rollout_path text,title text,archived integer,updated_at integer)')
    c.execute('insert into threads values (?,?,0,?)',(str(path),'原会话名称',int(clock[0])));c.commit()
    r=CodexUsageReader(tmp_path,now=lambda:clock[0],sources_path=tmp_path/'sources.json')
    meta=r.poll()[0];short=meta.short_id
    assert meta.session_title=='原会话名称'
    c.execute('update threads set title=?',('新会话名称',));c.commit();clock[0]+=31
    renamed=r.poll()[0]
    assert renamed.session_title=='新会话名称' and renamed.short_id==short
    restored=CodexUsageReader(tmp_path,now=lambda:clock[0],sources_path=tmp_path/'sources.json')
    assert restored.poll()[0].short_id==short
    c.close()


def test_same_project_identity_survives_new_turn_and_sort():
    s=SessionStore()
    for sid,title,code in [('a','修复提醒','A7C2'),('b','检查状态','B8D3')]:
        s.apply(CcHudEvent(1,sid,'state',turn_id='t',project='项目',state='thinking'),100)
        s.apply(CcHudEvent(1,sid,'metadata',turn_id='t',session_title=title,short_id=code),101)
    s.apply(CcHudEvent(1,'a','state',turn_id='next',state='tool'),102)
    rows=s.work_rows(103)
    assert [(r['session_title'],r['short_id']) for r in rows]==[('修复提醒','A7C2'),('检查状态','B8D3')]
    s.apply(CcHudEvent(1,'a','metadata',turn_id='next',session_title=''),104)
    assert s.work_rows(104)[0]['session_title']==''


def test_new_readback_preserves_identity_and_unknown_title():
    import struct
    raw=struct.pack('<BBBBBBII',1,2,2,0,0,4,17,65)
    for value in ('项目','model','high','Bash',''):
        b=value.encode();raw+=bytes([len(b)])+b
    raw+=bytes([37,0])
    for value in ('修复提醒','A7C2'):
        b=value.encode();raw+=bytes([len(b)])+b
    result=decode_work_readback(raw)
    assert result['session_title']=='修复提醒' and result['short_id']=='A7C2'
