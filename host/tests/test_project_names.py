import json
import sqlite3
from datetime import datetime, timezone

from cchud_daemon.project_names import ProjectNames
from cchud_daemon.codex_usage import CodexUsageReader
from cchud_daemon.event import CcHudEvent
from cchud_daemon.session_store import SessionStore


def database(home):
    c=sqlite3.connect(home/'state_5.sqlite')
    c.executescript('CREATE TABLE projects (id TEXT,name TEXT);'
                    'CREATE TABLE project_roots (project_id TEXT,path TEXT);'
                    'CREATE TABLE threads (id TEXT,project_id TEXT,rollout_path TEXT,title TEXT,archived INTEGER,updated_at INTEGER);')
    return c


def test_assignment_precedes_root_and_legacy_assignment_is_supported(tmp_path):
    c=database(tmp_path)
    c.execute('INSERT INTO projects VALUES (?,?)',('db','数据库项目名'))
    c.execute('INSERT INTO threads (id,project_id) VALUES (?,?)',('s','db'));c.commit()
    (tmp_path/'.codex-global-state.json').write_text(json.dumps({
        'local-projects':{'legacy':{'name':'福利数据看板','rootPaths':['/repo/welfare']}},
        'thread-project-assignments':{'s':{'projectKind':'local','projectId':'legacy'},
                                      'other':{'projectKind':'local','projectId':'legacy'}}}))
    names=ProjectNames(tmp_path);names.refresh()
    assert names.name('s','/repo/welfare')=='数据库项目名'
    assert names.name('other','/unrelated/worktree')=='福利数据看板'
    assert names.name('new','/repo/welfare/subdir')=='福利数据看板'
    assert names.name('new','/repo/welfare-other')==''
    c.close()


def test_ambiguous_root_falls_back_and_nested_project_is_preferred(tmp_path):
    c=database(tmp_path)
    for pid,name,root in [('a','项目甲','/repo'),('b','项目乙','/repo'),('c','子项目','/repo/child')]:
        c.execute('INSERT INTO projects VALUES (?,?)',(pid,name))
        c.execute('INSERT INTO project_roots VALUES (?,?)',(pid,root))
    c.commit();c.close()
    names=ProjectNames(tmp_path);names.refresh()
    assert names.name('s','/repo')==''
    assert names.name('s','/repo/child/src')=='子项目'


def test_project_display_name_survives_hooks_and_can_return_to_directory():
    s=SessionStore()
    s.apply(CcHudEvent(1,'s','metadata',turn_id='t',project='repo',project_name='福利数据看板'),100)
    s.apply(CcHudEvent(1,'s','state',turn_id='t',state='tool',project='repo'),101)
    assert s.work_rows(102)[0]['project']=='福利数据看板'
    s.apply(CcHudEvent(1,'s','state',turn_id='next',state='thinking',project='repo'),103)
    assert s.work_rows(104)[0]['project']=='福利数据看板'
    s.apply(CcHudEvent(1,'s','metadata',turn_id='next',project_name=''),105)
    assert s.work_rows(106)[0]['project']=='repo'


def test_project_rename_without_new_log_updates_metadata(tmp_path):
    now=datetime.now(timezone.utc);clock=[now.timestamp()]
    folder=tmp_path/'sessions';folder.mkdir()
    path=folder/'s.jsonl'
    rows=[{'type':'session_meta','payload':{'id':'s'}},
          {'type':'turn_context','payload':{'turn_id':'t','cwd':'/repo/welfare','model':'model'}}]
    path.write_text(''.join(json.dumps({**r,'timestamp':now.isoformat()})+'\n' for r in rows))
    c=database(tmp_path)
    c.execute('INSERT INTO projects VALUES (?,?)',('p','福利数据看板'))
    c.execute('INSERT INTO threads VALUES (?,?,?,?,0,?)',('s','p',str(path),'会话标题',int(clock[0])))
    c.commit()
    reader=CodexUsageReader(tmp_path,now=lambda:clock[0])
    assert reader.poll()[0].project_name=='福利数据看板'
    c.execute('UPDATE projects SET name=?',('新的项目名',));c.commit();clock[0]+=31
    assert reader.poll()[0].project_name=='新的项目名'
    c.execute('DELETE FROM projects');c.commit();clock[0]+=31
    assert reader.poll()[0].project_name==''
    c.close()
