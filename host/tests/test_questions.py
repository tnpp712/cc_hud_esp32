import json
from cchud_daemon.questions import QuestionTracker
from cchud_daemon.session_store import SessionStore
from cchud_daemon.event import CcHudEvent


def begin(q,sid='s',turn='t'):
    q.ingest(sid,{'type':'turn_context','payload':{'turn_id':turn}},1)


def ask(q,cid='call',count=1,async_=True,sid='s',ts=2):
    q.ingest(sid,{'type':'response_item','payload':{'type':'function_call','name':'request_user_input_async' if async_ else 'request_user_input','call_id':cid,'arguments':json.dumps({'questions':[{'title':'问题'}]*count})}},ts)


def accepted(q,cid='call',sid='s',ok=True):
    q.ingest(sid,{'type':'response_item','payload':{'type':'function_call_output','call_id':cid,'output':json.dumps({'accepted':ok})}},3)


def reply(q,cid='call',index=0,sid='s'):
    value=[{'questionItemId':json.dumps(['request_user_input_async',cid,index]),'answer':'已回答'}]
    q.ingest(sid,{'type':'response_item','payload':{'type':'message','role':'user','content':[{'type':'input_text','text':'<send_user_message_question_reply>'+json.dumps(value)+'</send_user_message_question_reply>'}]}},5)


def test_async_ack_is_not_answer_and_partial_reply_is_per_id():
    q=QuestionTracker();begin(q);ask(q,count=2)
    assert q.count('s')==0
    accepted(q);assert q.count('s')==2
    reply(q,index=0);assert q.ids('s')==['call:1']
    reply(q,index=0);assert q.count('s')==1
    reply(q,index=1);assert q.count('s')==0


def test_ordinary_message_or_tool_completion_does_not_clear_question():
    q=QuestionTracker();begin(q);ask(q);accepted(q)
    q.ingest('s',{'type':'response_item','payload':{'type':'function_call_output','call_id':'other','output':'done'}},4)
    q.ingest('s',{'type':'response_item','payload':{'type':'message','role':'user','content':[{'text':'继续'}]}},4)
    q.ingest('s',{'type':'event_msg','payload':{'type':'task_complete','turn_id':'t'}},4)
    assert q.count('s')==1


def test_cancel_is_scoped_and_survives_daemon_restart(tmp_path):
    path=tmp_path/'closures.json';q=QuestionTracker(path)
    begin(q);ask(q);accepted(q)
    begin(q,'other');ask(q,sid='other');accepted(q,sid='other')
    q.cancel('s','t',6,persist=True)
    assert q.count('s')==0 and q.count('other')==1
    restored=QuestionTracker(path);begin(restored);ask(restored);accepted(restored)
    assert restored.count('s')==0
    ask(restored,cid='new',ts=7);accepted(restored,cid='new')
    assert restored.count('s')==1
    restored.ingest('s',{'type':'event_msg','payload':{'type':'turn_aborted','turn_id':'t'}},8)
    assert restored.count('s')==0


def test_sync_result_and_failed_async_submission_clear():
    q=QuestionTracker();begin(q);ask(q,async_=False)
    assert q.count('s')==1
    accepted(q);assert q.count('s')==0
    ask(q,cid='bad');accepted(q,cid='bad',ok=False);assert q.count('s')==0


def test_reply_before_ack_does_not_reopen_question():
    q=QuestionTracker();begin(q);ask(q);reply(q);accepted(q)
    assert q.count('s')==0


def test_pending_question_keeps_phase_and_survives_idle_ttl():
    s=SessionStore(ttl_s=10)
    s.apply(CcHudEvent(1,'s','state',state='tool',phase='command',turn_id='t',project='项目'),100)
    s.apply(CcHudEvent(1,'s','metadata',turn_id='t',pending_questions=1,question_ids=['q:0'],ts=100),101)
    assert s.work_rows(200)[0]['phase']=='command'
    assert s.work_rows(200)[0]['state']=='tool'
    assert s.live_states(200)==[('waiting','pending-input','question')]
    s.apply(CcHudEvent(1,'s','metadata',pending_questions=0,question_ids=[]),201)
    assert s.work_rows(201)==[]


def test_pending_snapshot_can_restore_after_restart_without_inventing_work():
    s=SessionStore()
    s.apply(CcHudEvent(1,'s','metadata',turn_id='t',project='项目',pending_questions=1,ts=10),20)
    r=s.work_rows(20)[0]
    assert r['phase']=='unknown' and r['pending_questions']==1


def test_session_end_clears_all_questions_and_new_question_can_reopen(tmp_path):
    path=tmp_path/'closures.json';q=QuestionTracker(path)
    begin(q);ask(q);accepted(q)
    begin(q,turn='next');ask(q,cid='second',ts=4);accepted(q,cid='second')
    q.cancel('s','next',6,session_end=True,persist=True)
    assert q.count('s')==0
    restored=QuestionTracker(path);begin(restored);ask(restored);accepted(restored)
    assert restored.count('s')==0
    begin(restored,turn='resumed');ask(restored,cid='fresh',ts=7);accepted(restored,cid='fresh')
    assert restored.count('s')==1


def test_new_turn_plain_reply_expires_previous_prompt_without_affecting_other_session():
    q=QuestionTracker();begin(q);ask(q);accepted(q)
    begin(q,sid='other');ask(q,sid='other');accepted(q,sid='other')
    begin(q,turn='next')
    assert q.count('s')==1  # 单独开始轮次还不代表已收到用户输入。
    q.ingest('s',{'type':'response_item','payload':{'type':'message','role':'user',
        'content':[{'text':'确认推送到该公开仓'}]}},6)
    assert q.count('s')==0 and q.count('other')==1
    accepted(q)  # 迟到的提交回执不能复活旧提醒。
    assert q.count('s')==0
    ask(q,cid='new',ts=7);accepted(q,cid='new')
    assert q.ids('s')==['new:0']


def test_new_turn_structured_partial_reply_and_environment_keep_other_questions():
    q=QuestionTracker();begin(q);ask(q,count=2);accepted(q)
    begin(q,turn='next')
    for text in ('# AGENTS.md instructions for /repo\n规则', '<environment_context>环境</environment_context>'):
        q.ingest('s',{'type':'response_item','payload':{'type':'message','role':'user',
            'content':[{'text':text}]}},4)
    assert q.count('s')==2
    reply(q,index=0)
    assert q.ids('s')==['call:1']
