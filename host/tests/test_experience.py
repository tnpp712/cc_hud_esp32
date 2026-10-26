import struct
import pytest
from cchud_daemon.activity import ActivityTracker
from cchud_daemon.questions import QuestionTracker
from cchud_daemon.event import CcHudEvent
from cchud_daemon.session_store import SessionStore
from cchud_daemon.codec import encode_work_frames,decode_note_readback,TAG_WORK_NOTE
from cchud_daemon.daemon import Daemon
from cchud_daemon.aggregator import Aggregator
from test_questions import begin,ask,accepted,reply


def message(phase,text):
    return {'type':'response_item','payload':{'type':'message','role':'assistant','phase':phase,'content':[{'type':'output_text','text':text}]}}


def test_only_public_progress_and_final_are_used_and_turns_reset():
    a=ActivityTracker();a.ingest('s',{'type':'turn_context','payload':{'turn_id':'t'}},1)
    a.ingest('s',message('analysis','不能显示的推理'),2)
    assert a.snapshot('s')['progress']==''
    a.ingest('s',message('commentary','正在检查假期统计口径。'),3)
    assert a.snapshot('s')['progress']=='正在检查假期统计口径。'
    a.ingest('s',message('final_answer','已整理结果，仍需生产验收。'),4)
    assert a.snapshot('s')['outcome']==0
    a.ingest('s',{'type':'event_msg','payload':{'type':'task_complete','turn_id':'t'}},5)
    assert a.snapshot('s')['result']=='已整理结果，仍需生产验收。'
    a.ingest('s',{'type':'event_msg','payload':{'type':'task_started','turn_id':'next'}},6)
    assert a.snapshot('s')['result']=='' and a.snapshot('s')['progress']==''
    a.ingest('s',{'type':'event_msg','payload':{'type':'turn_aborted','turn_id':'next'}},7)
    assert a.snapshot('s')['outcome']==2


def test_verified_metrics_do_not_come_from_claims():
    a=ActivityTracker();a.ingest('s',{'type':'turn_context','payload':{'turn_id':'t'}},1)
    a.ingest('s',message('commentary','测试已经全部成功'),2)
    for kind,data in [('FileChange',{'status':'completed','changes':{'a':{},'b':{}}}),
                      ('CommandExecution',{'command':'pytest -q','exit_code':0}),
                      ('CommandExecution',{'command':'pytest failed','exit_code':1}),
                      ('CommandExecution',{'command':'echo tests passed','exit_code':0})]:
        a.ingest('s',{'type':'event_msg','payload':{'type':'item_completed','item':{'type':kind,**data}}},3)
    a.ingest('s',{'type':'event_msg','payload':{'type':'task_complete','turn_id':'t'}},4)
    assert a.snapshot('s')['result']=='变更 2 个文件；测试命令 1/2 成功'


def test_pending_title_follows_partial_reply_and_clear():
    q=QuestionTracker();begin(q)
    import json
    q.ingest('s',{'type':'response_item','payload':{'type':'function_call','name':'request_user_input_async','call_id':'call',
        'arguments':json.dumps({'questions':[{'title':'确认统计口径'},{'title':'确认发布范围'}]})}},2)
    accepted(q);assert q.attention('s')=='确认统计口径'
    reply(q,index=0);assert q.attention('s')=='确认发布范围'
    reply(q,index=1);assert q.attention('s')==''


def test_recent_result_survives_next_turn_and_expires_without_stale_work():
    s=SessionStore(ttl_s=10)
    s.apply(CcHudEvent(1,'s','state',state='thinking',turn_id='t',started_at=100),100)
    s.apply(CcHudEvent(1,'s','state',state='idle',turn_id='t',ended_at=120,outcome=1),120)
    s.apply(CcHudEvent(1,'s','metadata',turn_id='t',ended_at=120,outcome=1,result='等待验收'),121)
    s.apply(CcHudEvent(1,'s','state',state='tool',turn_id='next',started_at=125),125)
    rows=s.work_rows(126)
    assert len(rows)==2 and rows[1]['history'] and rows[1]['result']=='等待验收'
    assert rows[1]['elapsed']==20 and rows[0]['outcome']==0
    assert s.counts(126)==(1,1)
    assert s.work_rows(2000)==[]


def test_approval_attention_is_not_erased_by_empty_question_metadata():
    s=SessionStore()
    s.apply(CcHudEvent(1,'s','state',state='waiting',turn_id='t',intervention_kind='approval',attention='确认发布范围'),100)
    s.apply(CcHudEvent(1,'s','metadata',turn_id='t',pending_questions=0,attention=''),101)
    assert s.work_rows(102)[0]['attention']=='确认发布范围'
    s.apply(CcHudEvent(1,'s','metadata',turn_id='t',outcome=3,ended_at=103,result='命令失败'),103)
    assert Aggregator().aggregate(s,104)[2]=='error'


def test_cards_are_separate_bounded_utf8_frames_and_readback():
    row=dict(key=17,state='tool',progress='检查指标口径'*100,attention='确认范围'*100,result='结果摘要'*100,outcome=1,ended_at=123,result_key=456)
    frames=encode_work_frames([row],8)
    assert len(frames)==4 and all(len(f)<=244 for f in frames)
    for part,frame in enumerate(frames[1:],1):
        assert frame[13]==TAG_WORK_NOTE
        data=frame[15:];assert data[5]==part and len(data)==16+data[15]
        data[16:].decode('utf8')
    raw=struct.pack('<BIBII',1,17,1,123,456)
    for text in ('检查口径','确认范围','结果摘要'):
        b=text.encode();raw+=bytes([len(b)])+b
    assert decode_note_readback(raw)['attention']=='确认范围'
    with pytest.raises(ValueError):decode_note_readback(raw[:-1])


@pytest.mark.asyncio
async def test_duplicate_stop_does_not_repeat_completion_pulse(tmp_path):
    class Link:
        use_v7=True
        def __init__(self):self.frames=[]
        async def enqueue(self,payload):self.frames.append(payload)
    ble=Link();d=Daemon('addr',str(tmp_path/'s.sock'),ble=ble,now=lambda:100)
    event=CcHudEvent(1,'s','state',state='idle',detail='turn-complete',turn_id='t',ended_at=100,outcome=1)
    await d.on_events([event,event])
    assert sum(b'\x20\x01\x04' in frame for frame in ble.frames)==1
