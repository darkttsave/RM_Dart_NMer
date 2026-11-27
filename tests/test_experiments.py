import io
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'debug' / 'web'))
import app as backend


def test_records_without_devices_and_conflicts(tmp_path):
    backend.app.config.update(TESTING=True, EXPERIMENT_ROOT=str(tmp_path), RECORDER_CONTROL_PORT=0)
    client = backend.app.test_client()
    assert client.get('/experiments').status_code == 200
    assert client.post('/api/experiments', json={'title': ''}).status_code == 400
    payload = {'title': '阶段测试', 'sections': {'control': {'yaw': '0'}, 'vision': {'roi': '不适用'}},
               'shots': [{'landing_offset': '0', 'image_reference': '左上角 / 1440×1080', 'landing_reference': '靶心'}]}
    created = client.post('/api/experiments', json=payload).get_json()['record']
    assert created['sections']['control']['yaw'] == '0'
    assert created['sections']['vision']['roi'] == '不适用'
    assert created['shots'][0]['landing_reference'] == '靶心'
    assert client.get('/api/experiments/' + created['id']).get_json()['record'] == created
    payload.update(revision=created['revision'], status='confirmed')
    updated = client.put('/api/experiments/' + created['id'], json=payload)
    assert updated.status_code == 200
    assert updated.get_json()['record']['status'] == 'confirmed'
    assert client.put('/api/experiments/' + created['id'], json=payload).status_code == 409
    assert client.put('/api/experiments/' + created['id'], json={'title':'x','unknown':1,'revision':2}).status_code == 400
    assert client.get('/api/experiments/EXP-../../secret').status_code == 404


def test_attachment_and_video_reference(tmp_path):
    backend.app.config.update(TESTING=True, EXPERIMENT_ROOT=str(tmp_path))
    client = backend.app.test_client()
    record = client.post('/api/experiments', json={'title':'录像测试'}).get_json()['record']
    record_id = record['id']
    uploaded = client.post(f'/api/experiments/{record_id}/attachments', data={'file':(io.BytesIO(b'image'), '../../photo.png')}).get_json()['attachment']
    assert client.get(f'/api/experiments/{record_id}/attachments/{uploaded}').status_code == 404
    payload = {'title':'录像测试','revision':record['revision'],'attachments':[uploaded],'sessions':['session_1']}
    with patch.object(backend, 'experiment_sessions', side_effect=lambda: backend.jsonify(ok=True, sessions=[{'session':'session_1'}])):
        result = client.put('/api/experiments/'+record_id, json=payload)
    assert result.status_code == 200
    assert client.get(f'/api/experiments/{record_id}/attachments/{uploaded}').data == b'image'
    payload['revision'] = result.get_json()['record']['revision']
    # Existing session can be retained while playback is disconnected.
    with patch.object(backend, 'experiment_sessions', side_effect=AssertionError('must not contact playback')):
        assert client.put('/api/experiments/'+record_id, json=payload).status_code == 200
    payload['revision'] += 1
    payload['sessions'].append('missing')
    with patch.object(backend, 'experiment_sessions', side_effect=lambda: backend.jsonify(ok=True, sessions=[])):
        assert client.put('/api/experiments/'+record_id, json=payload).status_code == 400
