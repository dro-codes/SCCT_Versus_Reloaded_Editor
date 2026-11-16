"""Writes the image fixtures tests/ImageCodecTests.cpp decodes.

Run from the repository root with Pillow installed:
    python3 -I tests/mcp/fixtures/make_image_fixtures.py

Every image is the same 37x23 pattern (odd sizes reach the filters' edge
cases) saved by Pillow, which compresses with real zlib (dynamic Huffman
blocks and adaptive row filters). Each <name>.rgba holds what the decoder must
return: a 'RGBA' tag, width and height (little-endian uint32) and the pixels,
rows top to bottom.
"""
import gzip
import struct
import zlib
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
W, H = 37, 23


def pattern():
    image = Image.new("RGBA", (W, H))
    pixels = image.load()
    for y in range(H):
        for x in range(W):
            r = (x * 7) % 256
            g = (y * 11) % 256
            b = ((x ^ y) * 13) % 256
            a = 255 if (x + y) % 5 else (x * y) % 256
            if 10 <= x < 20 and 5 <= y < 12:
                r, g, b = 200, 30, 40  # a flat block, which LZ77 matches
            pixels[x, y] = (r, g, b, a)
    return image


def expect(name, image):
    rgba = image.convert("RGBA")
    data = b"RGBA" + struct.pack("<II", rgba.width, rgba.height) + rgba.tobytes()
    (HERE / (name + ".rgba")).write_bytes(data)


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def main():
    base = pattern()
    base.save(HERE / "rgba8.png", optimize=True)
    expect("rgba8", base)
    rgb = base.convert("RGB")
    rgb.save(HERE / "rgb8.png", compress_level=9)
    expect("rgb8", rgb)
    grey = base.convert("L")
    grey.save(HERE / "grey8.png")
    expect("grey8", grey)
    mono = base.convert("1")
    mono.save(HERE / "grey1.png")
    expect("grey1", mono)
    la = base.convert("LA")
    la.save(HERE / "greya8.png")
    expect("greya8", la)
    # 16 colours at 4 bits per pixel, with one transparent palette entry.
    pal = rgb.quantize(colors=16)
    pal.info["transparency"] = 3
    pal.save(HERE / "palette4.png", bits=4, transparency=3)
    expect("palette4", Image.open(HERE / "palette4.png"))
    # 16-bit RGB: the decoder keeps the high byte of each sample.
    raw = b"".join(
        b"\x00" + b"".join(struct.pack(">HHH", r * 257 + 3, g * 257, b * 257) for (r, g, b) in
                         (rgb.getpixel((x, y)) for x in range(W)))
        for y in range(H))
    ihdr = struct.pack(">IIBBBBB", W, H, 16, 2, 0, 0, 0)
    png16 = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    (HERE / "rgb16.png").write_bytes(png16)
    expect("rgb16", rgb)
    # Interlaced: refused with a clear message.
    ihdr = struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 1)
    (HERE / "interlaced.png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\x00" * 40)) + chunk(b"IEND", b""))
    rgb.save(HERE / "rgb24.bmp")
    expect("rgb24", rgb)
    base.save(HERE / "rgba32.tga")
    expect("rgba32", base)
    rgb.save(HERE / "rgb24rle.tga", compression="tga_rle")
    # gzip (Tiled's layer compression): a repeated line, fixed mtime.
    (HERE / "lines.gz").write_bytes(gzip.compress(b"".join(b"line %d of the gzip fixture\n" % i for i in range(200)), mtime=0))


if __name__ == "__main__":
    main()
