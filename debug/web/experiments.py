"""Stage experiment records; independent from the camera and Recorder."""
import fcntl
import json
import os
import re
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

ID_RE = re.compile(r'^EXP-[0-9a-f]{12}$')
SECTIONS = {'environment', 'vision', 'control', 'mechanical', 'conclusion'}
SHOT_FIELDS = {'dart_id', 'shot_number', 'fixed_compensation', 'compensation_applied',
               'green_center', 'target_yaw', 'actual_yaw', 'image_intersection',
               'image_reference', 'landing_offset', 'landing_reference', 'distance', 'hit', 'anomaly', 'observation'}
MAX_FILE = 2 * 1024 * 1024


def now():
    return datetime.now(timezone.utc).isoformat()


def validate(data):
    if not isinstance(data, dict):
        raise ValueError('记录必须是对象')
    allowed = {'title', 'date', 'purpose', 'status', 'sections', 'shots', 'sessions', 'attachments', 'revision'}
    if set(data) - allowed:
        raise ValueError('包含未知字段')
    if not isinstance(data.get('title'), str) or not data['title'].strip() or len(data['title']) > 200:
        raise ValueError('请填写不超过 200 字的实验标题')
    for key in ('date', 'purpose', 'status'):
        if key in data and (not isinstance(data[key], str) or len(data[key]) > 4000):
            raise ValueError(f'{key} 格式错误')
    if data.get('status', 'draft') not in ('draft', 'confirmed'):
        raise ValueError('状态无效')
    sections = data.get('sections', {})
    if not isinstance(sections, dict) or set(sections) - SECTIONS or any(not isinstance(v, dict) for v in sections.values()):
        raise ValueError('实验区块格式错误')
    fields = {
        'environment': {'site','distance','lighting','background','interference'},
        'vision': {'camera','detection','roi','result','snapshot_note'},
        'control': {'yaw','charge','settings','anomaly'},
        'mechanical': {'dart_id','version','parameters','state'},
        'conclusion': {'observation','analysis','issues','next_steps'},
    }
    for section, values in sections.items():
        if set(values) - fields[section] or any(not isinstance(v, str) or len(v) > 4000 for v in values.values()):
            raise ValueError(f'{section} 包含无效字段')
    shots = data.get('shots', [])
    if not isinstance(shots, list) or len(shots) > 1000:
        raise ValueError('发射明细格式错误')
    for shot in shots:
        if not isinstance(shot, dict) or set(shot) - SHOT_FIELDS:
            raise ValueError('发射明细包含无效字段')
        for key, value in shot.items():
            if key == 'observation':
                if not isinstance(value, dict) or len(json.dumps(value, ensure_ascii=False)) > 12000:
                    raise ValueError('观测数据格式错误')
                if set(value) != {'session', 'event'} or not isinstance(value['session'], str) or not isinstance(value['event'], dict):
                    raise ValueError('观测数据必须指定录像及事件')
                event = value['event']
                if event.get('type') != 'dart_reference_crossing' or event.get('feature') != 'bbox_center':
                    raise ValueError('观测事件类型无效')
            elif not isinstance(value, str) or len(value) > 1000:
                raise ValueError('发射明细包含无效字段')
    sessions = data.get('sessions', [])
    if not isinstance(sessions, list) or len(sessions) > 100 or any(not isinstance(s, str) or not re.fullmatch(r'[\w.\-]{1,150}', s) or s in ('.', '..') for s in sessions):
        raise ValueError('录像会话编号无效')
    attachments = data.get('attachments', [])
    if not isinstance(attachments, list) or any(not isinstance(s, str) or not re.fullmatch(r'[0-9a-f]{32}(?:\.[a-zA-Z0-9]{1,10})?', s) for s in attachments):
        raise ValueError('附件编号无效')
    if len(json.dumps(data, ensure_ascii=False).encode()) > MAX_FILE:
        raise ValueError('记录过大')
    return data


class Store:
    def __init__(self, root):
        self.root = Path(root).expanduser().absolute()

    def path(self, record_id):
        if not isinstance(record_id, str) or not ID_RE.fullmatch(record_id):
            raise ValueError('实验编号无效')
        path = self.root / record_id
        if path.is_symlink() or (path.exists() and not path.is_dir()):
            raise ValueError('实验目录无效')
        return path

    def read(self, record_id):
        path = self.path(record_id) / 'record.json'
        if path.is_symlink():
            raise ValueError('记录路径无效')
        with path.open(encoding='utf-8') as f:
            return json.load(f)

    def listing(self):
        if not self.root.exists():
            return []
        records = []
        for path in self.root.iterdir():
            if ID_RE.fullmatch(path.name) and path.is_dir() and not path.is_symlink():
                try:
                    rec = self.read(path.name)
                    records.append({k: rec.get(k) for k in ('id','title','date','status','updated_at','revision')})
                except (OSError, ValueError, json.JSONDecodeError):
                    continue
        return sorted(records, key=lambda r: r.get('updated_at') or '', reverse=True)

    def save(self, payload, record_id=None):
        validate(payload)
        self.root.mkdir(parents=True, exist_ok=True)
        # Advisory lock also protects updates from other Flask workers.
        with (self.root / '.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if record_id:
                path = self.path(record_id)
                old = self.read(record_id)
                if payload.get('revision') != old.get('revision'):
                    raise Conflict('记录已被修改，请重新读取后再保存')
            else:
                record_id = 'EXP-' + uuid4().hex[:12]
                path = self.path(record_id)
                path.mkdir(exist_ok=False)
                old = {}
            for attachment in payload.get('attachments', []):
                folder = path / 'attachments'
                candidate = folder / attachment
                if folder.is_symlink() or candidate.is_symlink() or not candidate.is_file():
                    raise ValueError('附件不存在')
            record = dict(payload)
            record.update(id=record_id, schema_version=1, created_at=old.get('created_at', now()),
                          updated_at=now(), revision=old.get('revision', 0) + 1, modified_by='web')
            fd, temporary = tempfile.mkstemp(prefix='.record-', dir=path)
            try:
                with os.fdopen(fd, 'w', encoding='utf-8') as f:
                    json.dump(record, f, ensure_ascii=False, indent=2, allow_nan=False)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(temporary, path / 'record.json')
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
            return record


class Conflict(Exception):
    pass
