#!/usr/bin/env python3
"""Compare serial/parallel telemetry on identical moving-target video frames."""
import json, os, re, signal, socket, struct, subprocess, tempfile
from pathlib import Path
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]

def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

def read_exact(sock, n):
    data = b''
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise EOFError('debug stream ended')
        data += part
    return data

def normalized(frame):
    keys = ('detected', 'tracking', 'target_x', 'target_y', 'radius', 'turn_flag',
            'distance_flag', 'lock_frames', 'lost_frames', 'green_pixels', 'candidate_count', 'tx')
    result = {k: frame.get(k) for k in keys}
    dart = dict(frame['dart'])
    dart.pop('timestamp_us', None)
    dart.pop('processing_ms', None)
    dart['track_points'] = [{k: v for k, v in pt.items() if k != 'timestamp_us'}
                            for pt in dart.get('track_points', [])]
    result['dart'] = dart
    return result

def run(binary, folder, video, parallel):
    data_port, control_port = port(), port()
    work = folder / ('parallel' if parallel else 'serial')
    (work / 'config').mkdir(parents=True)
    (work / 'build').mkdir()
    conf = (ROOT / 'config/debug.yaml').read_text()
    conf = re.sub(r'(?m)^(  port:)\s*5010\b', rf'\g<1> {data_port}', conf)
    conf = re.sub(r'(?m)^(  control_port:)\s*5011\b', rf'\g<1> {control_port}', conf)
    conf = re.sub(r'(?m)^(  publish_fps:)\s*\d+', r'\g<1> 0', conf)
    conf = re.sub(r'(dart_detection:\s*\n\s*enabled:)\s*(true|false)', r'\1 true', conf)
    (work / 'config/debug.yaml').write_text(conf)
    with socket.socket() as server, open(work / 'run.log', 'w') as log:
        server.bind(('127.0.0.1', data_port))
        server.listen(1)
        server.settimeout(20)
        args = [str(binary), '--source', f'video:{video}', '--no-serial', '--no-gui', '--no-record']
        if parallel:
            args.append('--parallel-detection')
        proc = subprocess.Popen(args, cwd=work / 'build', stdout=log, stderr=log)
        frames = {}
        try:
            conn, _ = server.accept()
            with conn:
                conn.settimeout(20)
                while True:
                    magic, version, size, jpeg_size = struct.unpack('<4sB3xII', read_exact(conn, 16))
                    assert magic == b'DWDB' and version == 1
                    data = json.loads(read_exact(conn, size))
                    jpeg = read_exact(conn, jpeg_size)
                    seq = data['seq']
                    if seq <= 350:
                        frames[seq] = (normalized(data), jpeg)
                    if seq >= 350:
                        break
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        assert proc.returncode == 0, (parallel, proc.returncode)
        return frames

def main():
    with tempfile.TemporaryDirectory(prefix='dart-parallel-compare-') as tmp:
        folder = Path(tmp)
        video = folder / 'source.avi'
        writer = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*'MJPG'), 30, (640, 480))
        assert writer.isOpened()
        for i in range(80):
            image = np.zeros((480, 640, 3), np.uint8)
            if i % 30 < 23:
                cv2.circle(image, (250 + i % 20, 220), 18, (0, 255, 0), -1)
            cv2.rectangle(image, (20 + i * 5, 330), (39 + i * 5, 340), (0, 0, 255), -1)
            writer.write(image)
        writer.release()
        serial = run(ROOT / 'build/main', folder, video, False)
        parallel = run(ROOT / 'build/main', folder, video, True)
        common = sorted(serial.keys() & parallel.keys())
        assert len(common) >= 20, len(common)
        for seq in common:
            assert serial[seq] == parallel[seq], f'serial/parallel mismatch at frame {seq}'
        assert any(serial[seq][0]['detected'] for seq in common), 'no green target tested'
        assert any(serial[seq][0]['dart']['track_valid'] for seq in common), 'no dart track tested'
        print(f'PASS {len(common)} common frames: green, dart, serial output and debug JPEG match')

if __name__ == '__main__':
    main()
