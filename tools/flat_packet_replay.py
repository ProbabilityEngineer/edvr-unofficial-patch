"""Replay captured inputs offline; WARP by default, explicit hardware comparison optional.

Comparison preserves captured MRTs and shaders. It compares replay bytes to the
captured executed result, NOT to an original-game counterfactual. A zero-write
packet gives weak coverage even when all bytes match. No legacy trace is validated.
"""
import argparse
import json
import pathlib
import struct
import subprocess
import tempfile

MAX_METADATA = 64 << 20

def shader_hash(paths):
    value = 1469598103934665603
    for path in paths:
        with path.open('rb') as source:
            while block := source.read(1 << 20):
                for byte in block:
                    value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value

def leaf(root, name):
    p = pathlib.Path(name)
    if p.is_absolute() or '..' in p.parts or '/' in name or '\\' in name:
        raise ValueError('unsafe capture leaf')
    path = (root / p).resolve()
    if path.parent != root.resolve():
        raise ValueError('capture leaf escapes directory')
    return path

def prepare(packet, pipeline='original', comparison=False, wine_paths=False):
    if packet.stat().st_size > MAX_METADATA:
        raise ValueError('metadata cap')
    p = json.loads(packet.read_text(encoding='utf-8'))
    if p['schema'] != 'edvr-flat-draw-packet' or p['version'] != 1:
        raise ValueError('packet schema')
    if pipeline not in ('original', 'executed'):
        raise ValueError('unknown pipeline')
    if pipeline != 'original' and not comparison:
        raise ValueError('executed pipeline requires comparison mode')
    if comparison and (not p['complete'] or not p.get('executed')):
        raise ValueError('comparison requires a complete executed packet')
    fields = {f['name']: f for f in p['original' if pipeline == 'original' else 'executed_pipeline']}
    if comparison and any(f['status'] != 'complete' for f in fields.values()):
        raise ValueError('comparison pipeline metadata incomplete')
    if not comparison and any('OM.RTV' + str(i) + '.resource' in fields for i in range(1, 8)):
        raise ValueError('diagnostic currently supports a single original render target')
    # This diagnostic deliberately assesses original state separately from legacy trace q.
    required = {int(f['value']) for f in fields.values() if f['type'] == 'resource-id' and f['value'] != 'null'}
    after_names = ['OM.RTV0.after', 'OM.DSV.after']
    if comparison:
        after_names += [f'OM.RTV{i}.after' for i in range(1, 8) if f'OM.RTV{i}.resource' in fields]
    for name in after_names:
        matches = [r['id'] for r in p['resources'] if r['name'] == name]
        if len(matches) != 1:
            raise ValueError('required after snapshot absent or duplicated: ' + name)
        required.add(matches[0])
    resources = []
    total = 0
    for r in p['resources']:
        if r['id'] not in required:
            continue
        if r['status'] != 'complete':
            raise ValueError('required original resource incomplete: ' + r['name'])
        parts = []
        for s in r['subresources']:
            if s['status'] != 'complete':
                raise ValueError('required subresource incomplete')
            paths = [leaf(packet.parent, f) for f in s['files']]
            if sum(f.stat().st_size for f in paths) != s['bytes']:
                raise ValueError('payload size mismatch')
            if s['bytes'] != s['depth_pitch'] * s['depth'] or s['depth_pitch'] != s['row_pitch'] * s['rows']:
                raise ValueError('packed payload layout mismatch')
            total += s['bytes']
            if total > 1536 << 20:
                raise ValueError('resource budget')
            parts.append((s, paths))
        resources.append((r, parts))
    shaders = []
    for stage in ('VS', 'PS'):
        f = fields[stage + '.bytecode']
        if f['status'] != 'complete':
            raise ValueError('original shader bytecode incomplete')
        paths = [leaf(packet.parent, n) for n in f['value'].split(';')]
        if sum(x.stat().st_size for x in paths) > MAX_METADATA:
            raise ValueError('shader cap')
        expected_hash = p[stage.lower()] if pipeline == 'original' else fields[stage + '.hash']['value']
        if shader_hash(paths) != int(expected_hash, 16):
            raise ValueError('original shader hash mismatch')
        shaders.append(paths)
    out = bytearray(b'EDVRRP01')
    def u(v): out.extend(struct.pack('<I', v))
    def string(v):
        b = str(v).encode('utf-8'); u(len(b)); out.extend(b)
    def source_path(v):
        string('Z:' + str(v).replace('/', '\\') if wine_paths else v)
    u(p['draw']['count']); u(p['draw']['start']); out.extend(struct.pack('<i', p['draw']['base']))
    u(p['draw']['instances']); u(p['draw']['start_instance'])
    if p['draw']['kind'] != 'X':
        raise ValueError('currently requires indexed-instanced draw')
    u(len(fields))
    for name, f in fields.items(): string(name); string(f['value'])
    u(len(resources))
    for r, parts in resources:
        u(r['id']); string(r['name']); string(r['type']); string(r['desc_hex']); u(len(parts))
        for s, paths in parts:
            for k in ('subresource', 'row_pitch', 'depth_pitch', 'rows', 'depth', 'bytes'): u(s[k])
            u(len(paths))
            for path in paths: source_path(path)
    for paths in shaders:
        u(len(paths))
        for path in paths: source_path(path)
    return out, {'frame': p['frame'], 'q': p['q'], 'vs': p['vs'], 'ps': p['ps'],
                 'resource_bytes': total, 'trace_validated': False, 'packet_complete': p['complete'],
                 'pipeline': pipeline, 'comparison': comparison}

def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        # Windows TEMP may use an 8.3 alias; match leaf()'s resolved paths.
        root = pathlib.Path(tmp).resolve()
        assert leaf(root, 'a.bin') == root / 'a.bin'
        for name in ('../x', '/x', 'a/b', 'a\\b'):
            try: leaf(root, name)
            except ValueError: pass
            else: raise AssertionError('escape accepted')
        # Dry-run runs preparation but creates no recipe, directory or output.
        capture = root / 'capture'; capture.mkdir()
        packet = capture / 'p.json'; blob = capture / 'b.bin'; blob.write_bytes(b'1234')
        resource = {'id': 0, 'name': 'OM.RTV0.after', 'status': 'complete', 'type': 'D3D11_BUFFER_DESC', 'desc_hex': '',
                    'subresources': [{'subresource': 0, 'status': 'complete', 'row_pitch': 4, 'depth_pitch': 4, 'rows': 1, 'depth': 1, 'bytes': 4, 'files': ['b.bin']}]}
        r2 = dict(resource, id=1, name='OM.DSV.after')
        raw_hash = format(shader_hash([blob]), 'x')
        data = {'schema': 'edvr-flat-draw-packet', 'version': 1, 'frame': 1, 'q': 2, 'vs': raw_hash, 'ps': raw_hash, 'complete': False,
                'draw': dict(kind='X', count=3, start=0, base=0, instances=1, start_instance=0),
                'original': [dict(name=s+'.bytecode', type='text', status='complete', value='b.bin') for s in ('VS','PS')], 'resources': [resource,r2]}
        packet.write_text(json.dumps(data)); before = sorted(root.rglob('*'))
        originals = {p: p.read_bytes() for p in before if p.is_file()}
        run = subprocess.run([__import__('sys').executable, __file__, str(packet), '--output', str(root/'absent'/'result'), '--dry-run'], capture_output=True)
        assert run.returncode == 0 and sorted(root.rglob('*')) == before, run.stderr
        assert all(p.read_bytes() == content for p, content in originals.items())
        blob.write_bytes(b'4321')
        try: prepare(packet)
        except ValueError as e: assert 'shader hash' in str(e)
        else: raise AssertionError('corrupt shader accepted')
        blob.write_bytes(b'1')
        try: prepare(packet)
        except ValueError: pass
        else: raise AssertionError('truncation accepted')
        blob.write_bytes(b'1234')
        data.update(complete=True, executed=True)
        data['resources'] += [dict(resource, id=2, name='OM.RTV1'),
                              dict(resource, id=3, name='OM.RTV1.after')]
        data['original'].append(dict(name='OM.RTV1.resource', type='resource-id', status='complete', value='2'))
        data['executed_pipeline'] = json.loads(json.dumps(data['original']))
        patched = capture / 'patched.bin'
        patched.write_bytes(b'5678')
        for stage in ('VS', 'PS'):
            data['executed_pipeline'].append(dict(name=stage+'.hash', type='text', status='complete',
                                                  value=format(shader_hash([patched]), 'x')))
            next(f for f in data['executed_pipeline'] if f['name'] == stage+'.bytecode')['value'] = 'patched.bin'
        packet.write_text(json.dumps(data))
        try: prepare(packet)
        except ValueError as e: assert 'single original render target' in str(e)
        else: raise AssertionError('legacy solid-mask route accepted MRTs')
        original, info = prepare(packet, comparison=True)
        executed, executed_info = prepare(packet, pipeline='executed', comparison=True, wine_paths=True)
        assert original != executed and b'patched.bin' in executed and b'Z:\\' in executed
        assert info['pipeline'] == 'original' and executed_info['pipeline'] == 'executed'
        before = {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
        dry = subprocess.run([__import__('sys').executable, __file__, str(packet),
                              '--comparison', '--pipeline', 'executed', '--hardware', '--wine-paths',
                              '--output', str(root/'absent'/'comparison'), '--dry-run'], capture_output=True)
        assert dry.returncode == 0 and not (root/'absent').exists(), dry.stderr
        assert before == {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
        data['executed_pipeline'][-1]['value'] = '0'
        packet.write_text(json.dumps(data))
        try: prepare(packet, pipeline='executed', comparison=True)
        except ValueError as e: assert 'shader hash' in str(e)
        else: raise AssertionError('corrupt executed shader accepted')
        # Restore the legacy fixture for its missing-payload check.
        data['original'] = data['original'][:-1]
        packet.write_text(json.dumps(data))
        blob.unlink()
        try: prepare(packet)
        except OSError: pass
        else: raise AssertionError('missing payload accepted')
    print('flat packet replay Python self-test: PASS')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packet', nargs='?', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--runner', type=pathlib.Path)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--comparison', action='store_true', help='exact captured multi-target draw; no solid-mask experiment')
    parser.add_argument('--pipeline', choices=('original', 'executed'), default='original')
    parser.add_argument('--hardware', action='store_true', help='explicit hardware request instead of WARP; comparison only')
    parser.add_argument('--wine-paths', action='store_true', help='encode source paths through Wine Z: mapping')
    a = parser.parse_args()
    if a.self_test: self_test(); return 0
    if not a.packet or not a.output: parser.error('packet and --output required')
    if a.hardware and not a.comparison: parser.error('--hardware requires --comparison')
    recipe, summary = prepare(a.packet.resolve(), a.pipeline, a.comparison, a.wine_paths)
    if a.output.resolve().is_relative_to(a.packet.resolve().parent):
        raise ValueError('output must be outside the source capture')
    print(json.dumps(summary))
    if a.dry_run: return 0
    if not a.runner: parser.error('--runner required except --dry-run')
    if a.comparison and a.output.exists():
        raise ValueError('comparison output must be a fresh directory')
    a.output.mkdir(parents=True, exist_ok=True)
    path = a.output/'replay.recipe'; path.write_bytes(recipe)
    flags = (['--comparison'] if a.comparison else []) + (['--hardware'] if a.hardware else [])
    return subprocess.run([str(a.runner.resolve()), str(path.resolve()), str(a.output.resolve()), *flags]).returncode

if __name__ == '__main__':
    try: raise SystemExit(main())
    except (ValueError, KeyError, OSError) as e: print('flat packet replay:', e); raise SystemExit(1)
