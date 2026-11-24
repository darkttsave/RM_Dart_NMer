#!/usr/bin/env python3
"""P2-B 本地进程、参数、事件和录像集成测试；不修改工作区配置。"""
import csv, json, os, re, signal, socket, subprocess, sys, tempfile, time
from pathlib import Path
import cv2
import numpy as np
import requests

ROOT=Path(__file__).resolve().parents[1]
def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def wait(fn,seconds=20):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try:
            r=fn()
            if r:return r
        except (OSError,ValueError,KeyError,requests.RequestException):pass
        time.sleep(.1)
    raise AssertionError('timeout')
def stop(p):
    if p and p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try:p.wait(timeout=15)
        except subprocess.TimeoutExpired:p.kill();p.wait()

def main():
 with tempfile.TemporaryDirectory(prefix='dart-p2b-') as t:
    t=Path(t);(t/'config').mkdir();(t/'build').mkdir(); dp,cp,wp=port(),port(),port()
    conf=(ROOT/'config/debug.yaml').read_text()
    conf=re.sub(r'(?m)^(  port:)\s*5010\b',rf'\g<1> {dp}',conf)
    conf=re.sub(r'(?m)^(  control_port:)\s*5011\b',rf'\g<1> {cp}',conf)
    conf=re.sub(r'(dart_detection:\s*\n\s*enabled:)\s*(true|false)',r'\1 true',conf)
    (t/'config/debug.yaml').write_text(conf)
    video=t/'source.avi';writer=cv2.VideoWriter(str(video),cv2.VideoWriter_fourcc(*'MJPG'),30,(320,240));assert writer.isOpened()
    for i in range(35):
        img=np.zeros((240,320,3),np.uint8);cv2.rectangle(img,(20+i*5,120),(39+i*5,130),(0,0,255),-1);writer.write(img)
    writer.release(); log=open(t/'run.log','w'); processes=[]
    def start_main():
        p=subprocess.Popen([str(ROOT/'build/main'),'--source',f'video:{video}','--no-gui','--no-serial','--no-auto-record','--record-root',str(t/'rec')],cwd=t/'build',stdout=log,stderr=log);processes.append(p);return p
    flask=subprocess.Popen([sys.executable,str(ROOT/'debug/web/app.py'),'--port',str(wp),'--data-port',str(dp),'--control-port',str(cp)],cwd=ROOT,stdout=log,stderr=log);processes.append(flask)
    proc=start_main();base=f'http://127.0.0.1:{wp}'
    def get(path):return requests.get(base+path,timeout=5).json()
    def post(path,payload):return requests.post(base+path,json=payload,timeout=8).json()
    def frame():return get('/api/status').get('frame') or {}
    def dart():return frame().get('dart') or {}
    try:
        wait(lambda:get('/api/params').get('ok'))
        wait(lambda:dart().get('enabled') and dart().get('candidate_count',0)>0)
        first=get('/api/params');assert first['running']['dart_color']==0
        # 整组非法值必须拒绝，其他有效值不能部分生效。
        before=first['running']['dart_value_min']
        assert not post('/api/params',{'cmd':'apply','values':{'dart_color':True}})['ok']
        assert not post('/api/params',{'cmd':'apply','values':{'dart_color':'green'}})['ok']
        assert not post('/api/params',{'cmd':'apply','values':{'dart_close_kernel':4,'dart_value_min':before+1}})['ok']
        assert get('/api/params')['running']['dart_value_min']==before
        assert not post('/api/params',{'cmd':'apply','values':{'dart_min_area':9000,'dart_max_area':10}})['ok']
        # 实际视觉候选数量随阈值即时变化。
        assert post('/api/params',{'cmd':'apply','values':{'dart_saturation_min':255,'dart_value_min':255}})['ok']
        # MJPG 压缩可能保留纯红像素，因此以面积上限验证可重复的视觉变化。
        assert post('/api/params',{'cmd':'apply','values':{'dart_min_area':900,'dart_close_kernel':5}})['ok']
        wait(lambda:dart().get('enabled') and dart().get('candidate_count')==0)
        assert post('/api/params',{'cmd':'apply','values':{'dart_min_area':20,'dart_saturation_min':100,'dart_value_min':80,'dart_max_distance':100}})['ok']
        wait(lambda:dart().get('candidate_count',0)>0)
        tracked=wait(lambda: (d if (d:=dart()).get('track_valid') else None))
        previous_frame=tracked['frame_id']
        assert post('/api/params',{'cmd':'apply','values':{'dart_max_distance':90}})['ok']
        wait(lambda: (d if (d:=dart()).get('track_points') and d['track_points'][0]['frame_id']>=previous_frame else None))
        print('PASS 跟踪阈值更新后旧轨迹已重置')
        status=post('/api/recorder',{'cmd':'start','record_image_mode':'raw'});assert status['ok'];session=Path(status['session_dir'])
        wait(lambda:get('/api/recorder').get('video_frames',0)>=8)
        before_reset=dart()['frame_id']
        assert post('/api/dart/reset',{})['ok']
        wait(lambda: (d if (d:=dart()).get('track_points') and d['track_points'][0]['frame_id']>=before_reset else None))
        print('PASS 手动请求在后续帧清除旧轨迹')
        if os.environ.get('DART_TEST_UI')=='1':
            subprocess.run(['node',str(ROOT/'tests/test_dart_runtime_ui.js'),base+'/'],check=True,env=os.environ)
        else:
            assert post('/api/params',{'cmd':'apply','values':{'dart_color':1}})['ok']
            assert post('/api/dart/reset',{})['ok']
        wait(lambda:dart().get('color')=='blue' and dart().get('candidate_count')==0)
        assert post('/api/params',{'cmd':'save'})['ok']
        assert 'color: blue' in (t/'config/debug.yaml').read_text()
        wait(lambda:get('/api/recorder').get('video_frames',0)>=20)
        # Web 断开后控制口和录像继续工作，重连后页面重新接收遥测。
        before_disconnect=get('/api/recorder')['video_frames']
        stop(flask)
        def direct_status():
            with socket.create_connection(('127.0.0.1',cp),timeout=2) as sock:
                sock.sendall(b'{"cmd":"status"}\n')
                return json.loads(sock.recv(65536))
        wait(lambda:direct_status()['video_frames']>before_disconnect+5)
        flask=subprocess.Popen([sys.executable,str(ROOT/'debug/web/app.py'),'--port',str(wp),'--data-port',str(dp),'--control-port',str(cp)],cwd=ROOT,stdout=log,stderr=log);processes.append(flask)
        wait(lambda:dart().get('color')=='blue')
        print('PASS Flask 断开期间录像继续，重连后遥测恢复')
        done=post('/api/recorder',{'cmd':'stop'});assert done['ok']
        events=[json.loads(x) for x in (session/'events.jsonl').read_text().splitlines()]
        assert any(e['type']=='dart_tracker_reset' for e in events),events
        assert any(e.get('parameter')=='dart_color' for e in events),events
        with open(session/'telemetry_001.csv') as f: rows=list(csv.DictReader(f))
        with open(session/'video_index_001.csv') as f: indices=list(csv.DictReader(f))
        assert rows and indices and any(r['dart_enabled']=='1' for r in rows)
        assert all((r['dart_detected']=='1') or (r['dart_center_x']=='' and r['dart_center_y']=='') for r in rows)
        ids={r['frame_id']:r for r in rows}
        assert all(r['source_frame_id'] in ids and r['source_elapsed_us']==ids[r['source_frame_id']]['elapsed_us'] for r in indices)
        cap=cv2.VideoCapture(str(session/'raw_001.avi'));assert cap.isOpened() and cap.read()[0];cap.release()
        print('PASS 热更新、原子校验、录像诊断、事件和索引')
        stop(proc);proc=start_main()
        wait(lambda:get('/api/params').get('running',{}).get('dart_color')==1)
        wait(lambda:dart().get('color')=='blue')
        assert post('/api/params',{'cmd':'apply','values':{'dart_color':'red'}})['ok']
        wait(lambda:dart().get('color')=='red')
        print('PASS YAML 保存及重启恢复，颜色 blue；API 支持 red 字符串')
    finally:
        for p in reversed(processes):stop(p)
        log.close()
if __name__=='__main__':main()
