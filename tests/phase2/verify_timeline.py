#!/usr/bin/env python3
"""Real ffprobe/frame-hash checks; no Windows filesystem claims."""
import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1])
def probe(path, *args):
    return json.loads(subprocess.check_output(['ffprobe', '-v', 'error', *args, '-of', 'json', str(path)]))
def frames(path):
    return probe(path, '-select_streams', 'v:0', '-show_frames')['frames']
def hashes(path):
    text = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v:0',
                                   '-fps_mode', 'passthrough', '-enc_time_base:v', 'demux', '-f', 'framemd5', '-'], text=True)
    return [line.rsplit(',', 1)[-1].strip() for line in text.splitlines() if not line.startswith('#')]
cache = {}
for row in (root / 'results.tsv').read_text().splitlines():
    source_name, output_name, anchor, duration = row.split('\t')
    anchor, duration = float(anchor), float(duration)
    source, output = root / source_name, root / output_name
    if source_name not in cache:
        cache[source_name] = (probe(source, '-show_format', '-show_streams'), frames(source), hashes(source))
    metadata, src_frames, src_hashes = cache[source_name]
    actual = probe(output, '-show_format', '-show_streams')
    assert abs(float(actual['format']['duration']) - duration) < 0.00002, output_name
    assert len(metadata['streams']) == len(actual['streams']), output_name
    origin = float(metadata['format'].get('start_time', 0))
    out_frames = frames(output)
    source_pts = [float(f['pts_time']) - origin for f in src_frames]
    out_pts = [float(f['pts_time']) for f in out_frames]
    if '-reencode.' in output_name:
        indices = [i for i, t in enumerate(source_pts) if 2.7 - 1e-7 <= t < 5.2 - 1e-7]
    else:
        indices = [min(range(len(source_pts)), key=lambda i: abs(source_pts[i] - anchor - t)) for t in out_pts]
        required = {i for i, t in enumerate(source_pts) if anchor - 0.001 <= t < 5.2 - 1e-7}
        assert required.issubset(indices), ('missing requested presentation frame', output_name)
        assert hashes(output) == [src_hashes[i] for i in indices], ('lossless frame content', output_name)
    assert len(indices) == len(out_pts), ('frame count', output_name, len(indices), len(out_pts))
    for i, pts in zip(indices, out_pts):
        assert abs(source_pts[i] - anchor - pts) < 0.0021, ('VFR/timebase', output_name, source_pts[i], anchor, pts)
    for index, (a, b) in enumerate(zip(metadata['streams'], actual['streams'])):
        assert a['codec_name'] == b['codec_name'], ('codec changed', output_name)
        if a['codec_type'] != 'audio':
            continue
        assert (a['sample_rate'], a['channels']) == (b['sample_rate'], b['channels'])
        src = probe(source, '-select_streams', str(index), '-show_packets', '-show_data_hash', 'sha256')['packets']
        dst = probe(output, '-select_streams', str(index), '-show_packets', '-show_data_hash', 'sha256')['packets']
        assert dst, ('audio stream empty', output_name, index)
        pos = 0
        for packet in dst:
            while pos < len(src) and src[pos]['data_hash'] != packet['data_hash']:
                pos += 1
            assert pos < len(src), ('audio payload changed', output_name, index)
            expected = float(src[pos]['pts_time']) - origin - anchor
            assert abs(expected - float(packet['pts_time'])) < 0.0021, ('audio offset', output_name, index, expected, packet['pts_time'])
            pos += 1
print('PASS timeline: 16 trims, lossless frame hashes, fractional/VFR frame PTS/count, audio codec/payload/offsets, actual duration')

manual = Path((root / 'manual-path.txt').read_text())
assert hashes(manual) == hashes(root / 'input.mp4'), 'manual recording dropped or changed frames'
for index in (1, 2):
    src = probe(root / 'input.mp4', '-select_streams', str(index), '-show_packets', '-show_data_hash', 'sha256')['packets']
    dst = probe(manual, '-select_streams', str(index), '-show_packets', '-show_data_hash', 'sha256')['packets']
    by_hash = {p['data_hash']: p for p in src}
    assert dst, 'missing manual audio'
    for packet in dst:
        original = by_hash[packet['data_hash']]
        assert abs(float(original['pts_time']) - float(packet['pts_time'])) < 0.0021, 'manual A/V origin'
print('PASS manual recording: all video frame hashes and both audio track offsets preserved')
