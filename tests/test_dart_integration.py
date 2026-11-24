#!/usr/bin/env python3
"""隔离进程验证视频回绕、结构化遥测与 P1 原始/调试录像。"""
import json, os, re, signal, socket, struct, subprocess, sys, tempfile, threading, time
from pathlib import Path
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/main'

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0)); return s.getsockname()[1]

def wait(predicate, timeout=15):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        try:
            value=predicate()
            if value:return value
        except (OSError,ValueError,KeyError): pass
        time.sleep(.05)
    raise AssertionError('timed out')

def recv_exact(sock, n):
    data=b''
    while len(data)<n:
        chunk=sock.recv(n-len(data))
        if not chunk: raise EOFError
        data+=chunk
    return data

def main():
    with tempfile.TemporaryDirectory(prefix='dart-p2-') as tmp:
        tmp=Path(tmp);(tmp/'config').mkdir();(tmp/'build').mkdir()
        data_port,control_port=port(),port()
        conf=(ROOT/'config/debug.yaml').read_text()
        conf=re.sub(r'(?m)^(  port:)\s*5010\b',rf'\g<1> {data_port}',conf)
        conf=re.sub(r'(?m)^(  control_port:)\s*5011\b',rf'\g<1> {control_port}',conf)
        conf=re.sub(r'(?m)^(  enabled:) false(\s*\n  draw_debug:)',r'\g<1> true\2',conf)
        assert 'enabled: true\n  draw_debug:' in conf
        (tmp/'config/debug.yaml').write_text(conf)
        video=tmp/'source.avi'
        writer=cv2.VideoWriter(str(video),cv2.VideoWriter_fourcc(*'MJPG'),30,(320,240));assert writer.isOpened()
        for i in range(30):
            img=np.zeros((240,320,3),np.uint8)
            cv2.rectangle(img,(30+5*i,120),(47+5*i,129),(0,0,255),-1)
            cv2.rectangle(img,(260,30),(275,41),(0,0,255),-1)
            writer.write(img)
        writer.release()
        server=socket.socket();server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        server.bind(('127.0.0.1',data_port));server.listen(1);server.settimeout(15)
        log=open(tmp/'process.log','w')
        proc=subprocess.Popen([str(BINARY),'--source','video:'+str(video),'--no-serial','--no-gui',
            '--no-auto-record','--record-root',str(tmp/'rec')],cwd=tmp/'build',stdout=log,stderr=log)
        collected=[]; stop=threading.Event(); reader=None; conn=None
        try:
            conn,_=server.accept();conn.settimeout(3)
            def receive():
                try:
                    while not stop.is_set():
                        header=recv_exact(conn,16); magic,version,n,j=struct.unpack('<4sB3xII',header)
                        assert magic==b'DWDB' and version==1
                        data=json.loads(recv_exact(conn,n));recv_exact(conn,j)
                        collected.append(data)
                except (OSError,EOFError): pass
            reader=threading.Thread(target=receive,daemon=True);reader.start()
            def control(command):
                with socket.create_connection(('127.0.0.1',control_port),timeout=2) as s:
                    s.sendall(json.dumps(command).encode()+b'\n');return json.loads(s.recv(65536))
            wait(lambda:control({'cmd':'status'}).get('ok'))
            wait(lambda:len(collected)>30)
            sessions={}
            for mode in ('raw','debug'):
                status=control({'cmd':'start','record_image_mode':mode});assert status['ok'],status
                wait(lambda:control({'cmd':'status'}).get('video_frames',0)>=10)
                assert control({'cmd':'stop'})['ok']
                path=Path(status['session_dir'])/(mode+'_001.avi')
                cap=cv2.VideoCapture(str(path));ok,frame=cap.read();cap.release();assert ok
                # 输入此区域全黑，只有调试画面上的 Dart 状态文字会修改它。
                bright=np.count_nonzero(np.max(frame[72:96,5:210],axis=2)>70)
                assert bright < 15 if mode=='raw' else bright > 50,(mode,bright)
                sessions[mode]=bright
            wait(lambda:any(x['dart']['track_state'] in ('captured','tracking') for x in collected))
            # 验证每帧中轨迹的媒体时间不跨越当前视频循环边界。
            darts=[x['dart'] for x in collected if x.get('dart',{}).get('enabled')]
            assert any(d['track_valid'] and len(d['track_points'])>=2 for d in darts)
            assert any(d['source_time_ms'] is not None and d['source_time_ms']<40 for d in darts)
            for d in darts:
                if not d['detected']: assert d['center'] is None and d['bbox'] is None
                pts=d['track_points']; times=[p['source_time_ms'] for p in pts]
                assert all(t is not None for t in times)
                assert times==sorted(times) and (not times or times[-1]<=d['source_time_ms']+0.1),d
                assert all(p['timestamp_us']>0 and p['frame_id']<=d['frame_id'] for p in pts)
            assert any(a['source_time_ms'] is not None and b['source_time_ms'] is not None and b['source_time_ms']<a['source_time_ms'] for a,b in zip(darts,darts[1:]))
            print('PASS 实际循环视频、结构化遥测和跨循环轨迹隔离',len(darts),'帧')
            print('PASS P1 录像标注隔离：原始/调试区域亮像素',sessions)
        finally:
            stop.set()
            if conn: conn.close()
            server.close()
            if proc.poll() is None:proc.send_signal(signal.SIGTERM)
            try:proc.wait(timeout=15)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            if reader:reader.join(timeout=4)
            log.close()

if __name__=='__main__':main()
