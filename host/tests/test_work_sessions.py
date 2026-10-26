import struct
from cchud_daemon.adapters.codex import CodexAdapter
from cchud_daemon.event import CcHudEvent
from cchud_daemon.session_store import SessionStore
from cchud_daemon.codec import encode_work_frames, TAG_WORK_ENTRY_V3
from cchud_daemon.hook_payload import compact_payload


def event(name, call, tool='Bash', **extra):
    return {'event':name,'payload':{'session_id':'s','turn_id':'turn','tool_use_id':call,'tool_name':tool,**extra}}


def test_parallel_post_does_not_clear_other_command():
    a=CodexAdapter()
    a.normalize(event('PreToolUse','long'))
    a.normalize(event('PreToolUse','read','Read'))
    e=a.normalize(event('PostToolUse','read','Read'))[0]
    assert (e.state,e.detail,e.phase)==('tool','Bash','command')
    e=a.normalize(event('PostToolUse','long'))[0]
    assert e.state=='thinking' and e.detail=='Bash'


def test_automatic_approval_keeps_pending_command_and_interrupt_clears():
    a=CodexAdapter(reviewer_resolver=lambda p:'auto_review')
    a.normalize(event('PreToolUse','cmd'))
    e=a.normalize(event('PermissionRequest','permission'))[0]
    assert (e.state,e.detail,e.phase)==('tool','Bash','command')
    assert a.normalize(event('Interrupt',''))[0].state=='idle'
    assert a.normalize(event('PostToolUse','cmd'))==[]


def test_metadata_and_real_plan_survive_state_updates():
    store=SessionStore()
    store.apply(CcHudEvent(1,'s','metadata',turn_id='t',project='项目',model='model',effort='high',started_at=100,ts=100),100)
    store.apply(CcHudEvent(1,'s','state',turn_id='t',state='tool',detail='Bash',phase='test',plan=[{'step':'运行测试','status':'in_progress'}]),120)
    r=store.work_rows(140)[0]
    assert (r['project'],r['model'],r['elapsed'],r['plan_total'],r['step'])==('项目','model',40,1,'运行测试')
    store.apply(CcHudEvent(1,'s','state',turn_id='t',state='idle',ended_at=145),145)
    assert store.work_rows(150)[0]['elapsed']==45
    store.apply(CcHudEvent(1,'s','state',turn_id='next',state='thinking',started_at=160),160)
    assert not store.work_rows(161)[0]['plan_known']
    store.apply(CcHudEvent(1,'s','state',session_end=True),162)
    assert store.work_rows(162)==[]


def test_every_work_frame_fits_mtu_and_utf8_is_intact():
    row=dict(key=123,client_id=1,state='tool',kind='none',phase='command',time_known=True,plan_known=True,
             elapsed=23,plan_done=2,plan_total=4,project='项目'*40,model='model'*20,effort='high',detail='Bash',step='检查文件'*40,session_title='修复同一个项目的会话展示'*30,short_id='A'*12)
    frames=encode_work_frames([row]*8,7)
    assert len(frames)==8
    for index,frame in enumerate(frames):
        assert len(frame)<=244
        i=4
        while i<len(frame):
            tag,n=frame[i:i+2];v=frame[i+2:i+2+n];i+=n+2
            if tag!=TAG_WORK_ENTRY_V3:continue
            assert v[0]==index and struct.unpack_from('<I',v,1)[0]==123
            off=16
            for _ in range(5):
                n=v[off];off+=1;v[off:off+n].decode('utf8');off+=n
            off+=2
            for _ in range(2):
                n=v[off];off+=1;v[off:off+n].decode('utf8');off+=n
            assert off==len(v)


def test_compact_hook_preserves_pairing_but_not_command_or_response():
    p=compact_payload({'session_id':'s','tool_use_id':'id','cwd':'/project','model':'m',
        'tool_input':{'command':'pytest -q'},'tool_response':'large private output'})
    assert p['phase']=='test' and p['tool_use_id']=='id'
    assert 'tool_input' not in p and 'tool_response' not in p


def test_internal_automatic_reviewer_is_not_a_work_session():
    assert CodexAdapter().normalize(event('PreToolUse','review',model='codex-auto-review'))==[]


def test_model_metadata_is_not_lost_when_hook_arrives_first():
    store=SessionStore()
    store.apply(CcHudEvent(1,'s','state',turn_id='t',state='tool',project='project',model='model',phase='command'),110)
    store.apply(CcHudEvent(1,'s','metadata',turn_id='t',effort='high',started_at=100,ts=100),120)
    r=store.work_rows(140)[0]
    assert (r['project'],r['effort'],r['elapsed'],r['phase'])==('project','high',40,'command')


def test_readback_contract_and_short_mcp_name():
    from cchud_daemon.codec import decode_work_readback
    raw=struct.pack('<BBBBBBII',2,2,2,1,3,3,17,65)
    for value in ('项目','model','high','Bash','检查'):
        b=value.encode();raw+=bytes([len(b)])+b
    data=decode_work_readback(raw)
    assert (data['count'],data['key'],data['project'],data['model'],data['phase'])==(2,17,'项目','model',2)
    a=CodexAdapter()
    e=a.normalize(event('PreToolUse','mcp','mcp__docs__fetch_openai_doc'))[0]
    assert e.phase=='mcp' and e.detail=='fetch_openai_doc'


def test_context_and_pending_work_readback_and_session_isolation():
    from cchud_daemon.codec import decode_work_readback
    store=SessionStore()
    store.apply(CcHudEvent(1,'a','state',state='tool',phase='command',turn_id='ta'),100)
    store.apply(CcHudEvent(1,'b','state',state='thinking',turn_id='tb'),100)
    store.apply(CcHudEvent(1,'a','metadata',turn_id='ta',ctx_pct=42,effort='high',pending_questions=1),101)
    store.apply(CcHudEvent(1,'b','metadata',turn_id='tb',ctx_pct=7,effort='low',pending_questions=0),101)
    rows=store.work_rows(102)
    assert [(r['ctx_pct'],r['effort'],r['pending_questions']) for r in rows]==[(42,'high',1),(7,'low',0)]
    raw=struct.pack('<BBBBBBII',1,2,2,0,0,4,17,65)
    for value in ('项目','model','high','Bash',''):
        b=value.encode();raw+=bytes([len(b)])+b
    d=decode_work_readback(raw+bytes([42,1]))
    assert d['ctx_pct']==42 and d['pending_questions']==1
    assert d['state']==2 and d['phase']==2
