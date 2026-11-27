#!/usr/bin/env python3
"""T0.2 pseudo-terminal: byte framing, validity, timeout, web and recording."""
import csv, math, os, pty, re, select, signal, socket, struct, subprocess, sys, tempfile, threading, time
from pathlib import Path
import cv2
import numpy as np
import requests
ROOT = Path(__file__).resolve().parents[1]
def port():
    with socket.socket() as s: s.bind(('127.0.0.1', 0)); return s.getsockname()[1]
def packet(value, valid=1):
    payload = b'\x5b' + struct.pack('<fB', value, valid)
    crc = 0xffff
    for b in payload:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return payload + struct.pack('<H', crc)
def wait(fn, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = fn()
            if value: return value
        except (requests.RequestException, KeyError, ValueError, OSError): pass
        time.sleep(.025)
    raise AssertionError('timed out')
def main():
  with tempfile.TemporaryDirectory(prefix='dart-yaw-') as d:
    root = Path(d); (root/'config').mkdir(); (root/'build').mkdir(); (root/'rec').mkdir()
    master, slave = pty.openpty(); slave_name = os.ttyname(slave)
    dp, cp, wp = port(), port(), port()
    cfg = (ROOT/'config/debug.yaml').read_text().replace('port: "/dev/ttyACM0"', f'port: "{slave_name}"')
    cfg = re.sub(r'(?m)^(  port:)\s*5010\b', rf'\g<1> {dp}', cfg)
    cfg = re.sub(r'(?m)^(  control_port:)\s*5011\b', rf'\g<1> {cp}', cfg)
    cfg = re.sub(r'(?m)^(  watchdog:)\s*true', r'\1 false', cfg)
    cfg = re.sub(r'(?m)^(  yaw_timeout_ms:)\s*\d+', r'\1 400', cfg)
    (root/'config/debug.yaml').write_text(cfg)
    video = root/'source.avi'; writer = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*'MJPG'), 30, (320,240))
    assert writer.isOpened()
    for _ in range(30): writer.write(np.zeros((240,320,3), np.uint8))
    writer.release()
    log = open(root/'log','w'); procs = []; done = threading.Event(); tx = bytearray()
    def drain():
        while not done.is_set():
            try:
                if select.select([master], [], [], .05)[0]: tx.extend(os.read(master,4096))
            except OSError: break
    threading.Thread(target=drain,daemon=True).start()
    try:
        procs.append(subprocess.Popen([sys.executable,str(ROOT/'debug/web/app.py'),'--port',str(wp),'--data-port',str(dp),'--control-port',str(cp)],cwd=ROOT,stdout=log,stderr=log))
        procs.append(subprocess.Popen([str(ROOT/'build/main'),'--source',f'video:{video}','--no-gui','--no-auto-record','--record-root',str(root/'rec')],cwd=root/'build',stdout=log,stderr=log))
        base = f'http://127.0.0.1:{wp}'
        def get(path): return requests.get(base+path,timeout=2).json()
        def frame(): return get('/api/status').get('frame') or {}
        def status(value): return wait(lambda: (f if (f:=frame()).get('yaw_status') == value else None))
        status('waiting')
        result = requests.post(base+'/api/recorder',json={'cmd':'start','record_image_mode':'raw'},timeout=3).json(); assert result['ok']
        good = packet(-12.5); os.write(master,b'garbage'+good[:3]); time.sleep(.07); os.write(master,good[3:])
        f=status('valid'); assert f['yaw_actual_deg']==-12.5 and f['yaw_received_epoch_ms'] > 0
        bad=bytearray(packet(77));bad[-1] ^= 1;os.write(master,bad)
        time.sleep(.08); assert frame()['yaw_actual_deg'] != 77
        os.write(master,packet(4,0)); assert status('invalid')['yaw_actual_deg'] is None
        os.write(master,packet(float('nan'))); assert status('invalid')['yaw_actual_deg'] is None
        os.write(master,packet(21.25)); assert status('valid')['yaw_actual_deg']==21.25
        assert status('stale')['yaw_actual_deg'] is None
        result=requests.post(base+'/api/recorder',json={'cmd':'stop'},timeout=4).json();assert result['ok']
        path=next((root/'rec').glob('*/telemetry_001.csv'))
        with path.open() as f: rows=list(csv.DictReader(f))
        assert any(r['yaw_status']=='valid' and abs(float(r['yaw_actual_deg'])+12.5)<.01 for r in rows)
        assert any(r['yaw_status']=='invalid' and not r['yaw_actual_deg'] for r in rows)
        assert any(r['yaw_status']=='stale' and not r['yaw_actual_deg'] for r in rows)
        sys.path.insert(0, str(ROOT/'debug/web'))
        from playback import ReplaySession
        replay = ReplaySession(path.parent)
        assert any(r['yaw_status'] == 'valid' and r['yaw_actual_deg'] == 21.25 for r in replay.telemetry)
        assert any(r['yaw_status'] == 'stale' and r['yaw_actual_deg'] is None for r in replay.telemetry)
        assert len(tx) >= 15 and all(tx[i] == 0x5a for i in range(0, len(tx)-14, 15)) # existing 15-byte TX only
        print('PASS 串口分片/噪声/CRC/无效值/NaN/超时、Web 遥测、录像 CSV、历史回放')
    finally:
        done.set()
        for p in reversed(procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
        os.close(master);os.close(slave);log.close()
if __name__=='__main__':main()
