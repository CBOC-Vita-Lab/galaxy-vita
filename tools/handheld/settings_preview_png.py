#!/usr/bin/env python3
"""Convert native menu PPM previews to lossless PNG without external packages."""
import pathlib
import struct
import sys
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data) & 0xffffffff)


for path in pathlib.Path(sys.argv[1]).glob('*.ppm'):
    header, size, maximum, pixels = path.read_bytes().split(b'\n', 3)
    width, height = map(int, size.split())
    assert header == b'P6' and maximum == b'255' and len(pixels) == width*height*3
    rows = b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    path.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))+
                                        chunk(b'IDAT', zlib.compress(rows, 9))+chunk(b'IEND', b''))
