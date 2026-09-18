#!/usr/bin/env python3
"""Generate a small uint16 raw scan: four beams, end plates, and a streak."""
import array
from pathlib import Path
import sys


def main():
    nx, ny, nz = 64, 144, 400
    image = array.array('H', [0]) * (nx * ny * nz)

    def rectangle(z, x0, x1, y0, y1, value):
        row = array.array('H', [value]) * (x1-x0)
        for y in range(y0, y1):
            start = (z*ny+y)*nx+x0
            image[start:start+len(row)] = row

    for beam in range(4):
        y0 = 8+32*beam
        for z in range(20+4*beam, 380-8*beam):
            if 200 <= z < 206:
                continue
            rectangle(z, 20, 44, y0, y0+20, 800+100*beam)
    for z in list(range(2, 9)) + list(range(392, 400)):
        rectangle(z, 16, 48, 0, ny, 50000)
    rectangle(180, 20, 44, 8, 124, 1200)
    if sys.byteorder != 'little':
        image.byteswap()
    header = (f'NRRD0005\ntype: uint16\ndimension: 3\nsizes: {nx} {ny} {nz}\n'
              'encoding: raw\nendian: little\nspacings: 1 1 1\nunits: "mm" "mm" "mm"\n\n')
    output = Path(__file__).resolve().with_name('synthetic.nrrd')
    if output.exists():
        raise SystemExit(f'Refusing to overwrite {output}')
    with output.open('wb') as stream:
        stream.write(header.encode('ascii'))
        image.tofile(stream)
    print(output)


if __name__ == '__main__':
    main()
