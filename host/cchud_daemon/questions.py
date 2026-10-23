"""跟踪已确认的问题生命周期，不把异步提交回执当成用户回答。"""
from __future__ import annotations
import json
import re
from pathlib import Path
from threading import RLock

TOOLS = {'request_user_input', 'request_user_input_async'}
_REPLY = re.compile(r'<send_user_message_question_reply>\s*(.*?)\s*</send_user_message_question_reply>', re.S)


def _ordinary_user_input(text):
    """排除平台注入的工作区说明，避免把环境更新当成用户回复。"""
    text = text.strip()
    return bool(text) and not text.startswith((
        '# AGENTS.md instructions', '<environment_context>',
        '<permissions instructions>', '<INSTRUCTIONS>',
    )) and '<send_user_message_question_reply>' not in text


class QuestionTracker:
    def __init__(self, closure_path: Path | None = None):
        self.calls = {}
        self.turns = {}
        self.closed = {}
        self.closure_path = closure_path
        self.lock = RLock()
        if closure_path:
            try:
                value = json.loads(closure_path.read_text())
                if isinstance(value, dict): self.closed = value
            except (OSError, ValueError): pass

    def count(self, sid):
        with self.lock:
            return sum(len(c['open']) for c in self.calls.get(sid, {}).values() if c['accepted'])

    def ids(self, sid):
        with self.lock:
            return sorted(f'{cid}:{i}' for cid,c in self.calls.get(sid,{}).items()
                          if c['accepted'] for i in c['open'])

    def reset(self, sid):
        with self.lock:
            self.calls.pop(sid, None)
            self.turns.pop(sid, None)

    def cancel(self, sid, turn_id, timestamp, *, session_end=False, persist=False):
        with self.lock:
            calls = self.calls.get(sid, {})
            for cid in list(calls):
                if session_end or calls[cid]['turn'] == turn_id: calls.pop(cid)
            if persist:
                entry = self.closed.setdefault(sid, {'session_end':0,'turns':{}})
                if session_end: entry['session_end'] = max(entry.get('session_end',0), timestamp)
                elif turn_id: entry.setdefault('turns',{})[turn_id] = timestamp
                if len(entry.get('turns',{})) > 128:
                    entry['turns'] = dict(sorted(entry['turns'].items(), key=lambda x:x[1])[-128:])
                if self.closure_path:
                    self.closure_path.parent.mkdir(parents=True, exist_ok=True)
                    temp = self.closure_path.with_suffix('.tmp')
                    temp.write_text(json.dumps(self.closed))
                    temp.replace(self.closure_path)

    def _closed_before(self, sid, turn):
        entry = self.closed.get(sid,{})
        return max(entry.get('session_end',0),entry.get('turns',{}).get(turn,0))

    def ingest(self, sid, row, timestamp):
        with self.lock:
            p = row.get('payload') or {}
            if not isinstance(p,dict): return
            kind = p.get('type')
            calls = self.calls.setdefault(sid,{})
            if row.get('type')=='turn_context':
                self.turns[sid]=p.get('turn_id')
                return
            if row.get('type')=='event_msg':
                turn=p.get('turn_id') or self.turns.get(sid)
                if kind=='task_started': self.turns[sid]=turn
                elif kind in ('turn_aborted','session_end'):
                    self.cancel(sid,turn,timestamp,session_end=kind=='session_end')
                elif kind=='task_complete':
                    # 非阻塞追问可以在一轮结束后继续等待，不能被 Stop 一律清除。
                    for cid in list(calls):
                        if not calls[cid]['async'] and calls[cid]['turn']==turn: calls.pop(cid)
                return
            if row.get('type')!='response_item': return
            if kind in ('function_call','custom_tool_call'):
                name=str(p.get('name','')).split('.')[-1]
                if name not in TOOLS: return
                cid=p.get('call_id');turn=self.turns.get(sid)
                if not cid or timestamp<=self._closed_before(sid,turn): return
                try:
                    raw=p.get('arguments',p.get('input',{}));args=json.loads(raw) if isinstance(raw,str) else raw
                    questions=args.get('questions',[])
                except (ValueError,AttributeError): return
                if not isinstance(questions,list) or not questions: return
                if cid not in calls:
                    calls[cid]={'turn':turn,'async':name.endswith('_async'),
                                'accepted':not name.endswith('_async'),'open':set(range(len(questions)))}
            elif kind in ('function_call_output','custom_tool_call_output'):
                cid=p.get('call_id');call=calls.get(cid)
                if not call:return
                if not call['async']:
                    calls.pop(cid);return
                try:
                    out=p.get('output');out=json.loads(out) if isinstance(out,str) else out
                except ValueError:out=None
                if isinstance(out,dict) and out.get('accepted') is True:
                    call['accepted']=True
                    if not call['open']:calls.pop(cid,None)
                else:calls.pop(cid,None)
            elif kind=='message' and p.get('role')=='user':
                text=''.join(c.get('text','') for c in p.get('content',[]) if isinstance(c,dict))
                turn = self.turns.get(sid)
                if turn and _ordinary_user_input(text):
                    # 新一轮普通输入使旧轮次的提示过期，不猜测回答内容。
                    # 同轮追问和带问题 ID 的部分回答继续保留其余问题。
                    for cid in list(calls):
                        if calls[cid]['turn'] and calls[cid]['turn'] != turn:
                            calls.pop(cid)
                for match in _REPLY.finditer(text):
                    try: replies=json.loads(match.group(1))
                    except ValueError: continue
                    if not isinstance(replies,list):continue
                    for reply in replies:
                        try:
                            ident=json.loads(reply['questionItemId'])
                            name,cid,index=ident
                        except (ValueError,KeyError,TypeError):continue
                        if name not in TOOLS or not isinstance(index,int) or cid not in calls:continue
                        calls[cid]['open'].discard(index)
                        if not calls[cid]['open'] and calls[cid]['accepted']:calls.pop(cid)
