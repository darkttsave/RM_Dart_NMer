#!/usr/bin/env python3
"""Verify indexed seeking, backward seeks and sequential fallback."""
import sys
import tempfile
from pathlib import Path
from unittest import mock
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'debug/web'))
import playback

class TrackedCapture:
    def __init__(self, path, real_factory, fail_seek=False):
        self.cap = real_factory(path)
        self.fail_seek = fail_seek
        self.reads = 0
        self.seeks = []
    def isOpened(self): return self.cap.isOpened()
    def release(self): return self.cap.release()
    def get(self, prop): return self.cap.get(prop)
    def set(self, prop, value):
        self.seeks.append((prop, value))
        return False if self.fail_seek else self.cap.set(prop, value)
    def read(self):
        self.reads += 1
        return self.cap.read()

def main():
    with tempfile.TemporaryDirectory(prefix='dart-replay-seek-') as tmp:
        path = str(Path(tmp) / 'sample.avi')
        writer = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*'MJPG'), 30, (160, 120))
        assert writer.isOpened()
        for i in range(180):
            frame = np.full((120, 160, 3), (i % 256, (i * 3) % 256, (i * 7) % 256), np.uint8)
            cv2.putText(frame, str(i), (15, 65), cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2)
            writer.write(frame)
        writer.release()
        factory = cv2.VideoCapture
        reference = factory(path)
        expected = {}
        for index in (0, 1, 150, 12, 120):
            assert reference.set(cv2.CAP_PROP_POS_FRAMES, index)
            ok, frame = reference.read()
            assert ok
            ok, jpeg = cv2.imencode('.jpg', frame, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
            assert ok
            expected[index] = jpeg.tobytes()
        reference.release()
        captures = []
        def tracked(p):
            cap = TrackedCapture(p, factory)
            captures.append(cap)
            return cap
        with mock.patch.object(playback.cv2, 'VideoCapture', side_effect=tracked):
            reader = playback.VideoReader(path)
            for index in (0, 1, 150, 12, 120):
                assert reader.get_jpeg(index) == expected[index], index
            cap = captures[-1]
            assert cap.reads <= 6, cap.reads
            assert [int(value) for _, value in cap.seeks] == [150, 12, 120], cap.seeks
            reader._executor.submit(reader.cap.release).result()
            reader._executor.shutdown()
        captures = []
        def no_seek(p):
            cap = TrackedCapture(p, factory, fail_seek=True)
            captures.append(cap)
            return cap
        with mock.patch.object(playback.cv2, 'VideoCapture', side_effect=no_seek):
            reader = playback.VideoReader(path)
            assert reader.get_jpeg(120) == expected[120]
            assert captures[-1].reads == 121
            reader._executor.submit(reader.cap.release).result()
            reader._executor.shutdown()
        print('PASS indexed forward/backward seeking, sequential reads and unsupported-seek fallback')

if __name__ == '__main__':
    main()
