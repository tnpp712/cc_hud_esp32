#!/usr/bin/env python3
"""BLE 真机验收：版本回读、真实额度、状态和时间帧；结束恢复空闲。"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import time

from bleak import BleakClient
from cchud_daemon.codec import encode_v7_state, encode_time_v4, encode_work_frames, decode_work_readback
from cchud_daemon.codex_usage import CodexUsageReader
from cchud_daemon.daemon import Daemon
from cchud_daemon.event import CcHudEvent

DATA = '12345678-aaaa-bbbb-cccc-1234567890a1'
STATE = '12345678-aaaa-bbbb-cccc-1234567890a2'
VERSION = '12345678-aaaa-bbbb-cccc-1234567890a3'
WORK = '12345678-aaaa-bbbb-cccc-1234567890a4'


async def verify(address):
    async with BleakClient(address, timeout=20) as client:
        version = bytes(await client.read_gatt_char(VERSION)).decode().rstrip('\0')
        if version != '2026.10.23-codex.4':
            raise RuntimeError(f'设备版本不匹配：{version}')
        print(json.dumps({'固件版本':version,'BLE连接':bool(client.is_connected)},ensure_ascii=False),flush=True)
        class Link:
            use_v7 = True
            async def enqueue(self, payload):
                await client.write_gatt_char(DATA, payload, response=True)
                response = bytes(await client.read_gatt_char(STATE)).rstrip(b'\0')
                if response != b'OK':
                    raise RuntimeError(f'设备未确认：{response!r}')
        link = Link()
        daemon = Daemon(address, '/tmp/cchud-verify-unused.sock', ble=link)
        reader = CodexUsageReader(Path(os.environ.get('CODEX_HOME','~/.codex')).expanduser())
        events = reader.poll()
        await daemon.on_events(events or [CcHudEvent(1,'usage','quota',title='Codex',metrics_valid=0)])
        now = int(time.time())
        offset = -(time.altzone if time.localtime(now).tm_isdst > 0 else time.timezone)//60
        await link.enqueue(encode_time_v4(now, offset, ''))
        print('额度及校时：OK',flush=True)
        generation = int(time.time()*1000) & 0xFFFFFFFF
        try:
            for index,(state,detail,kind,label) in enumerate([(1,'','none','思考'),(2,'Bash','none','命令'),
                                           (4,'','none','完成脉冲')]):
                await link.enqueue(encode_v7_state(state,detail,1,int(state in (1,2,3)),kind))
                row=dict(key=0xC0DE,state={1:'thinking',2:'tool',3:'waiting',4:'idle'}[state],
                         kind=kind,phase={1:'thinking',2:'command',3:kind,4:'idle'}[state],
                         project='HUD 验收',model='--',effort='high',ctx_pct=42,detail=detail)
                for frame in encode_work_frames([row],generation+index): await link.enqueue(frame)
                actual=decode_work_readback(bytes(await client.read_gatt_char(WORK)))
                assert actual['project']=='HUD 验收' and actual['key']==0xC0DE
                assert actual['effort']=='high' and actual['ctx_pct']==42 and actual['pending_questions']==0
                assert actual['state']==(0 if state==4 else state)
                if state==2: assert actual['phase']==2 and actual['detail']=='Bash'
                print(f'{label}：设备工作信息回读 OK',flush=True)
                await asyncio.sleep(1.5)
        finally:
            await link.enqueue(encode_v7_state(0,'',0,0,'none'))
            for frame in encode_work_frames([],generation+100): await link.enqueue(frame)
        print('已恢复空闲，无测试会话残留。',flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address',required=True)
    asyncio.run(verify(parser.parse_args().address))
