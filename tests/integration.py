#!/usr/bin/env python3
"""Voxel-exact integration checks using only the Python standard library."""
import array
import json
import struct
import zlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()
REPLAY = Path(__file__).resolve().parents[1] / 'scripts/replay_crops.py'


def payload(values, endian):
    result = array.array('H', values)
    if sys.byteorder != endian:
        result.byteswap()
    return result.tobytes()


def read_nrrd(path):
    with path.open('rb') as stream:
        assert stream.readline().startswith(b'NRRD')
        fields = {}
        for line in stream:
            if not line.strip():
                break
            if not line.startswith(b'#') and b':=' not in line:
                key, value = line.decode().split(':', 1)
                fields[key] = value.strip()
        return fields, stream.read()


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='scan-separator-test-')
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def run_tool(self, source, *options, ok=True, output='out'):
        target = self.root / output
        result = subprocess.run([str(BINARY), str(source), '-o', str(target), *map(str, options)],
                                capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stderr)
        return target, result

    def assert_png(self, path):
        data = path.read_bytes()
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        pos, compressed, dimensions = 8, bytearray(), None
        while pos < len(data):
            length = struct.unpack('>I', data[pos:pos+4])[0]
            kind = data[pos+4:pos+8]
            body = data[pos+8:pos+8+length]
            checksum = struct.unpack('>I', data[pos+8+length:pos+12+length])[0]
            self.assertEqual(zlib.crc32(kind+body), checksum)
            if kind == b'IHDR':
                width, height, depth, color, _, _, _ = struct.unpack('>IIBBBBB', body)
                self.assertEqual((depth, color), (8, 2))
                dimensions = (width, height)
            elif kind == b'IDAT':
                compressed.extend(body)
            pos += length+12
        self.assertIsNotNone(dimensions)
        width, height = dimensions
        raw = zlib.decompress(compressed)
        self.assertEqual(len(raw), height*(1+3*width))
        self.assertGreater(width, 1000)
        self.assertGreater(height, 700)
        self.assertGreater(max(raw), 200)

    def test_box_image_can_be_disabled(self):
        source, _, _, _ = self.make_scan(count=1)
        target, _ = self.run_tool(source, '--preview', '--no-box-preview')
        self.assertTrue((target/'boxes.json').exists())
        self.assertFalse((target/'boxes_preview.png').exists())

    def make_scan(self, count=4, endian='little', signed=False, detached=False, spatial=False):
        nx, ny, nz = 96, 144, 480
        voxels = array.array('H', [0]) * (nx * ny * nz)
        beams = []
        material = 900 if signed else 45000  # exercises the unsigned high bit
        for i in range(count):
            y0, y1, z0, z1 = 10 + 32*i, 30 + 32*i, 20 + 4*i, 460 - 12*i
            beams.append(([30, y0, z0], [66, y1+3, z1]))
            for z in range(z0, z1):
                if 235 <= z < 243:  # an internal gap must not truncate the beam
                    continue
                drift = z // 140
                for y in range(y0+drift, y1+drift):
                    start = (z*ny+y)*nx+30
                    voxels[start:start+36] = array.array('H', [material])*36
        # Holder plates spanning all beam Y positions, separated by a short gap
        # that would attach them to the beam if closing happened before filtering.
        for z in list(range(2, 10)) + list(range(470, 478)):
            for y in range(ny):
                start = (z*ny+y)*nx+25
                voxels[start:start+46] = array.array('H', [30000])*46
        # A transverse bridge at one Z plane disappears under sparse 2D erosion.
        for y in range(8, 130):
            start = (210*ny+y)*nx+30
            voxels[start:start+36] = array.array('H', [material])*36
        if signed:
            # Negative values must stay background, rather than becoming 65000.
            for z in range(40, 440):
                for y in range(134, 142):
                    start = (z*ny+y)*nx+30
                    voxels[start:start+36] = array.array('H', [65500])*36
        header = (f'NRRD0005\ntype: {"int16" if signed else "uint16"}\ndimension: 3\n'
                  f'sizes: {nx} {ny} {nz}\nencoding: raw\nendian: {endian}\n'
                  'spacings: 1 1 1\nunits: "mm" "mm" "mm"\ncustom:=keep me\n')
        if spatial:
            header += ('space: left-posterior-superior\nspace directions: (0,1,0) (-1,0,0) (0,0,1)\n'
                       'space origin: (100,200,300)\n')
        source = self.root / 'scan.nrrd'
        if detached:
            header += 'data file: voxels.raw\nbyte skip: 6\n'
            (self.root/'voxels.raw').write_bytes(b'prefix' + payload(voxels, endian))
            source.write_bytes((header+'\n').replace('\n', '\r\n').encode())
        else:
            source.write_bytes((header+'\n').encode()+payload(voxels, endian))
        return source, voxels, beams, (nx, ny, nz)

    def assert_crops(self, source, target, voxels, shape, count):
        manifest = json.loads((target/'boxes.json').read_text())
        self.assertTrue(manifest['crops_written'])
        self.assertEqual(len(manifest['objects']), count)
        self.assertEqual(manifest['source_sizes'], list(shape))
        self.assertEqual(manifest['bounds'], 'min inclusive, max exclusive')
        self.assert_png(target/'boxes_preview.png')
        nx, ny, _ = shape
        centers = []
        for obj in manifest['objects']:
            lo, hi = obj['min'], obj['max']
            centers.append(lo[1]+hi[1])
            fields, actual = read_nrrd(target/obj['file'])
            self.assertEqual(list(map(int, fields['sizes'].split())), obj['size'])
            self.assertEqual(fields['encoding'], 'raw')
            self.assertEqual(fields['type'], manifest['source_type'])
            expected = array.array('H')
            for z in range(lo[2], hi[2]):
                for y in range(lo[1], hi[1]):
                    start = (z*ny+y)*nx+lo[0]
                    expected.extend(voxels[start:start+hi[0]-lo[0]])
            self.assertEqual(actual, payload(expected, fields['endian']))
        self.assertEqual(centers, sorted(centers, reverse=True))
        return manifest

    def test_automatic_four_beams_plates_gaps_exact_voxels(self):
        source, voxels, beams, shape = self.make_scan()
        target, _ = self.run_tool(source)
        manifest = self.assert_crops(source, target, voxels, shape, 4)
        for obj, (beam_lo, beam_hi) in zip(manifest['objects'], reversed(beams)):
            lo, hi = obj['min'], obj['max']
            for axis in (0, 1):
                self.assertLessEqual(lo[axis], beam_lo[axis])
                self.assertGreaterEqual(hi[axis], beam_hi[axis])
            self.assertLessEqual(lo[2], beam_lo[2]-3)
            self.assertGreaterEqual(hi[2], beam_hi[2]+3)
            self.assertGreaterEqual(lo[2], 10)
            self.assertLessEqual(hi[2], 470)
        # Count mismatch must not leave crops or a manifest behind.
        target, result = self.run_tool(source, '--count', 3, ok=False, output='wrong-count')
        self.assertIn('Expected 3 beams', result.stderr)
        self.assertFalse(target.exists())

        # Edit one box, remove all crops, and rerun the original command. The
        # manifest is now authoritative: segmentation and CLI margins are not
        # applied, custom metadata survives, and the preview is regenerated.
        edited = json.loads((self.root/'out/boxes.json').read_text())
        edited['objects'][0]['min'][0] -= 2
        edited['objects'][0]['max'][1] += 2
        edited['objects'][0]['size'] = [1, 1, 1]  # deliberately stale
        edited['manual_note'] = 'keep this field'
        edited['parameters']['manual_setting'] = 17
        expected_min = edited['objects'][0]['min'][:]
        expected_max = edited['objects'][0]['max'][:]
        (self.root/'out/boxes.json').write_text(json.dumps(edited, indent=2)+'\n')
        for crop in (self.root/'out').glob('*.nrrd'):
            crop.unlink()
        (self.root/'out/boxes_preview.png').write_bytes(b'old preview')
        replayed, result = self.run_tool(source, '--margin-mm', 99, '--no-box-preview')
        self.assertIn('Reusing edited boxes', result.stderr)
        self.assertIn('always regenerates boxes_preview.png', result.stderr)
        actual = self.assert_crops(source, replayed, voxels, shape, 4)
        self.assertEqual(actual['objects'][0]['min'], expected_min)
        self.assertEqual(actual['objects'][0]['max'], expected_max)
        self.assertEqual(actual['objects'][0]['size'],
                         [expected_max[i]-expected_min[i] for i in range(3)])
        self.assertEqual(actual['manual_note'], 'keep this field')
        self.assertEqual(actual['parameters']['manual_setting'], 17)
        self.assertEqual(actual['parameters']['margin_mm'], 5)
        self.assertTrue(actual['parameters']['manifest_replay'])

    def test_detached_end_plate_inside_margin_trims_box(self):
        source, voxels, _, shape = self.make_scan(count=1)
        nx, ny, _ = shape
        # Erase the generic upper plate, then add a detached plate alongside
        # the beam. Its Y position overlaps the crop margin, but not the wood.
        for z in range(470, 478):
            start = z*ny*nx
            voxels[start:start+ny*nx] = array.array('H', [0])*(ny*nx)
        for z in range(435, 465):
            for y in range(36, 52):
                start = (z*ny+y)*nx+30
                voxels[start:start+36] = array.array('H', [30000])*36
        with source.open('r+b') as stream:
            stream.seek(source.stat().st_size-len(voxels)*2)
            stream.write(payload(voxels, 'little'))
        guarded, result = self.run_tool(source, '--preview', '--trim-end-clutter', output='guarded')
        box = json.loads((guarded/'boxes.json').read_text())['objects'][0]
        self.assertLessEqual(box['max'][2], 435)
        self.assertIn('End guard', result.stderr)
        unguarded, _ = self.run_tool(source, '--preview', output='unguarded')
        box = json.loads((unguarded/'boxes.json').read_text())['objects'][0]
        self.assertGreaterEqual(box['max'][2], 463)
        self.assertFalse(json.loads((unguarded/'boxes.json').read_text())['parameters']['end_guard'])

    def test_default_end_margin_clips_at_scan_boundaries(self):
        source, _, _, shape = self.make_scan(count=1)
        nx, ny, nz = shape
        voxels = array.array('H', [0])*(nx*ny*nz)
        for z in range(nz):
            for y in range(10, 30):
                start = (z*ny+y)*nx+30
                voxels[start:start+36] = array.array('H', [900])*36
        with source.open('r+b') as stream:
            stream.seek(source.stat().st_size-len(voxels)*2)
            stream.write(payload(voxels, 'little'))
        target, _ = self.run_tool(source, '--preview')
        box = json.loads((target/'boxes.json').read_text())['objects'][0]
        self.assertEqual(box['min'][2], 0)
        self.assertEqual(box['max'][2], nz)

    def test_signed_big_endian_single_beam_and_rotated_origin(self):
        source, voxels, _, shape = self.make_scan(count=1, signed=True, endian='big', spatial=True)
        target, _ = self.run_tool(source, '--labels', 'my-beam')
        manifest = self.assert_crops(source, target, voxels, shape, 1)
        obj = manifest['objects'][0]
        self.assertEqual(obj['file'], 'my-beam.nrrd')
        fields, _ = read_nrrd(target/obj['file'])
        lo = obj['min']
        self.assertEqual(fields['space origin'], f'({100-lo[1]},{200+lo[0]},{300+lo[2]})')
        self.assertEqual(fields['endian'], 'big')
        self.assertIn(b'custom:=keep me', (target/obj['file']).read_bytes()[:1000])

    def test_preview_detached_crlf_labels_and_replay(self):
        source, voxels, _, shape = self.make_scan(detached=True)
        names = self.root/'labels.txt'
        names.write_text('high\nsecond\nthird\nlow\n')
        target, _ = self.run_tool(source, '--labels-file', names, '--preview')
        manifest = json.loads((target/'boxes.json').read_text())
        self.assertFalse(manifest['crops_written'])
        self.assertEqual(list(target.glob('*.nrrd')), [])
        self.assert_png(target/'boxes_preview.png')
        result = subprocess.run([sys.executable, str(REPLAY), str(target/'boxes.json'),
                                 '--binary', str(BINARY), '-o', str(self.root/'replay')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = self.assert_crops(source, self.root/'replay', voxels, shape, 4)
        self.assertEqual(actual['objects'], manifest['objects'])
        fields, _ = read_nrrd(self.root/'replay'/'high.nrrd')
        self.assertNotIn('data file', fields)
        self.assertNotIn('byte skip', fields)

    def test_explicit_full_box_no_off_by_one_and_no_overwrite(self):
        source, voxels, _, shape = self.make_scan(count=1)
        nx, ny, nz = shape
        target, _ = self.run_tool(source, '--box', f'0,0,0,{nx},{ny},{nz}')
        self.assert_crops(source, target, voxels, shape, 1)
        original = (target/'scan_1.nrrd').read_bytes()
        _, result = self.run_tool(source, '--box', f'0,0,0,{nx},{ny},{nz}', ok=False)
        self.assertIn('Delete all crop NRRDs', result.stderr)
        self.assertEqual((target/'scan_1.nrrd').read_bytes(), original)
        self.run_tool(source, '--box', f'0,0,0,{nx+1},{ny},{nz}', ok=False, output='invalid')

    def test_manifest_replay_rejects_invalid_bounds_without_rewriting(self):
        source, _, _, shape = self.make_scan(count=1)
        target, _ = self.run_tool(source, '--preview')
        data = json.loads((target/'boxes.json').read_text())
        data['objects'][0]['max'][0] = shape[0]+1
        edited = json.dumps(data, indent=2)+'\n'
        (target/'boxes.json').write_text(edited)
        preview = (target/'boxes_preview.png').read_bytes()
        _, result = self.run_tool(source, ok=False)
        self.assertIn('outside the source scan', result.stderr)
        self.assertEqual((target/'boxes.json').read_text(), edited)
        self.assertEqual((target/'boxes_preview.png').read_bytes(), preview)

    def test_invalid_input_and_names(self):
        source, _, _, _ = self.make_scan(count=1)
        self.run_tool(source, '--labels', '../escape', ok=False)
        self.run_tool(source, '--labels', 'a,b', '--count', 1, ok=False)
        self.run_tool(source, '--threshold', 'nan', ok=False)
        self.run_tool(source, '--step', 0, ok=False)
        self.run_tool(source, '--count', 0, ok=False)
        self.run_tool(source, '--box', '0,0,0,10,10,10', '--box', '0,0,0,10,10,10',
                      '--labels', 'same,same', ok=False)
        with source.open('r+b') as stream:
            stream.truncate(1000)
        _, result = self.run_tool(source, '--preview', ok=False)
        self.assertIn('Truncated', result.stderr)


if __name__ == '__main__':
    unittest.main()
