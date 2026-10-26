"""提取公开进展和本轮结果；不读取推理内容，不从自然语言猜测验收结论。"""
import re


def brief(value, limit=240):
    if not isinstance(value, str):
        return ''
    value = re.sub(r'```[\s\S]*?```', '', value)
    value = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', value)
    value = re.sub(r'https?://\S+', '', value)
    lines = [re.sub(r'^\s*(?:#{1,6}\s+|[-*>]\s+|\d+\.\s+)', '', line).strip().replace('**', '').replace('`', '')
             for line in value.splitlines() if line.strip()]
    return ' '.join(lines[:2])[:limit]


class ActivityTracker:
    def __init__(self):
        self.rows = {}

    def reset(self, sid):
        self.rows.pop(sid, None)

    def ingest(self, sid, row, timestamp):
        p = row.get('payload') or {}
        if not isinstance(p, dict):
            return
        typ, kind = row.get('type'), p.get('type')
        turn = p.get('turn_id')
        current = self.rows.setdefault(sid, {})
        if typ=='turn_context' or (typ=='event_msg' and kind=='task_started'):
            if turn and current.get('turn_id') != turn:
                current.clear()
                current.update(turn_id=turn, progress='', result='', outcome=0, ended_at=None,
                               files=set(), tests=[], final='')
            return
        if typ=='response_item' and kind=='message' and p.get('role')=='assistant':
            phase = p.get('phase') or p.get('channel')
            text = ''.join(x.get('text','') for x in p.get('content',[]) if isinstance(x,dict))
            if phase=='commentary':
                current['progress']=brief(text)
            elif phase in ('final', 'final_answer'):
                current['final']=brief(text)
                if current.get('outcome')==1:
                    current['result']=self._result(current)
        if typ!='event_msg':
            return
        if kind=='item_completed':
            item=p.get('item') or {}
            if item.get('type')=='FileChange' and item.get('status')=='completed':
                current.setdefault('files',set()).update((item.get('changes') or {}).keys())
            if item.get('type')=='CommandExecution' and item.get('exit_code') is not None:
                command=item.get('command','')
                if isinstance(command,list):command=' '.join(command)
                if isinstance(command,str) and re.search(r'\b(pytest|ctest|jest|vitest)\b|\b(npm|pnpm|cargo|pio)\s+test\b',command):
                    current.setdefault('tests',[]).append(item['exit_code']==0)
        if kind in ('task_complete','turn_aborted','task_failed') and (not turn or turn==current.get('turn_id')):
            current['outcome']={'task_complete':1,'turn_aborted':2,'task_failed':3}[kind]
            current['ended_at']=timestamp
            current['result']=(self._result(current) if kind=='task_complete' else
                               brief(p.get('message')) or ('本轮已中断' if kind=='turn_aborted' else '本轮执行失败'))

    @staticmethod
    def _result(row):
        parts=[]
        if row.get('files'):parts.append(f"变更 {len(row['files'])} 个文件")
        tests=row.get('tests',[])
        if tests:parts.append(f"测试命令 {sum(tests)}/{len(tests)} 成功")
        if row.get('final'):parts.append(row['final'])
        return '；'.join(parts) or '本轮结束，可在 Codex 查看结果'

    def snapshot(self, sid):
        row=self.rows.get(sid,{})
        return {key:row.get(key,default) for key,default in
                (('progress',''),('result',''),('outcome',0),('ended_at',None))}
