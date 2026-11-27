#!/usr/bin/env python3
"""P3 mock video: reference crossing -> recorder event -> replay and record association."""
import json, os, re, signal, socket, subprocess, sys, tempfile, time
from pathlib import Path
import cv2
import numpy as np
import requests
ROOT=Path(__file__).resolve().parents[1]
def port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def wait(fn, timeout=20):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        try:
            x=fn()
            if x: return x
        except (OSError,ValueError,KeyError,requests.RequestException): pass
        time.sleep(.1)
    raise AssertionError('timeout')
def stop(p):
    if p and p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try:p.wait(timeout=10)
        except subprocess.TimeoutExpired:p.kill();p.wait()
def main():
 with tempfile.TemporaryDirectory(prefix='dart-p3-') as tmp:
  root=Path(tmp);(root/'config').mkdir();(root/'build').mkdir();(root/'recordings').mkdir()
  dp,cp,wp,rp=port(),port(),port(),port()
  cfg=(ROOT/'config/debug.yaml').read_text()
  cfg=re.sub(r'(?m)^(  port:)\s*5010\b',rf'\g<1> {dp}',cfg)
  cfg=re.sub(r'(?m)^(  control_port:)\s*5011\b',rf'\g<1> {cp}',cfg)
  cfg=re.sub(r'(dart_detection:\s*\n(?:[^\n]*\n)*?  enabled:)\s*(true|false)',r'\1 true',cfg)
  cfg,count=re.subn(r'(  reference_line:\n    enabled:)\s*\d+\n    x1: \d+\n    y1: \d+\n    x2: \d+\n    y2: \d+',r'\1 1\n    x1: 100\n    y1: 0\n    x2: 100\n    y2: 239',cfg)
  assert count == 1
  (root/'config/debug.yaml').write_text(cfg)
  video=root/'source.avi';writer=cv2.VideoWriter(str(video),cv2.VideoWriter_fourcc(*'MJPG'),30,(320,240));assert writer.isOpened()
  for i in range(35):
   frame=np.zeros((240,320,3),np.uint8);cv2.rectangle(frame,(20+i*6,120),(39+i*6,130),(0,0,255),-1);writer.write(frame)
  writer.release(); log=open(root/'test.log','w');procs=[]
  def spawn(args,cwd=ROOT):
   p=subprocess.Popen(args,cwd=cwd,stdout=log,stderr=log);procs.append(p);return p
  try:
   spawn([sys.executable,str(ROOT/'debug/web/playback.py'),'--root',str(root/'recordings'),'--port',str(rp)])
   env=os.environ.copy();env['DART_EXPERIMENT_ROOT']=str(root/'experiments')
   p=subprocess.Popen([sys.executable,str(ROOT/'debug/web/app.py'),'--port',str(wp),'--data-port',str(dp),'--control-port',str(cp),'--playback-url',f'http://127.0.0.1:{rp}/'],cwd=ROOT,stdout=log,stderr=log,env=env);procs.append(p)
   spawn([str(ROOT/'build/main'),'--source',f'video:{video}','--no-gui','--no-serial','--no-auto-record','--record-root',str(root/'recordings')],root/'build')
   base=f'http://127.0.0.1:{wp}'
   def get(path):return requests.get(base+path,timeout=4).json()
   def post(path,payload):return requests.post(base+path,json=payload,timeout=4).json()
   wait(lambda:get('/api/params').get('ok'))
   assert get('/api/params')['running']['dart_reference_enabled']==1
   assert post('/api/recorder',{'cmd':'start','record_image_mode':'raw'})['ok']
   def eventfile():
    files=list((root/'recordings').glob('*/events.jsonl'))
    if not files:return None
    for line in files[0].read_text().splitlines():
     item=json.loads(line)
     if item.get('type')=='dart_reference_crossing':return item
   event=wait(eventfile)
   assert event['feature']=='bbox_center' and event['coordinate_system']=='full_image_top_left_px'
   assert abs(event['intersection'][0]-100)<.01 and event['line_id']=='A'
   assert event['sdk_frame_number'] is None and event['green_valid'] is False
   assert post('/api/params', {'cmd':'apply','values':{'dart_window_enabled':1,'dart_window_delay_ms':0,'dart_window_duration_ms':1000,'dart_window_tracking_ms':2000}})['ok']
   wait(lambda: (get('/api/status').get('frame') or {}).get('dart',{}).get('failure_reason') == 'window_closed')
   assert post('/api/dart/simulate-fire',{})['simulated']
   wait(lambda: (get('/api/status').get('frame') or {}).get('dart',{}).get('track_valid'))
   assert post('/api/recorder',{'cmd':'stop'})['ok']
   data=wait(lambda: (x if (x:=get('/api/experiment-sessions')).get('sessions') else None))
   session=data['sessions'][0];obs=next(x for x in session['events'] if x['type']=='dart_reference_crossing')
   record=post('/api/experiments',{'title':'P3 测试','sessions':[session['session']],'shots':[{'shot_number':'1','observation':{'session':session['session'],'event':obs}}]})
   assert record['ok'] and record['record']['shots'][0]['observation']['event']['intersection']==event['intersection']
   print('PASS 模拟视频穿越参考线 → 原始录像事件 → 历史回放 → 指定发次实验记录')
  finally:
   for p in reversed(procs):stop(p)
   log.close()
if __name__=='__main__': main()
