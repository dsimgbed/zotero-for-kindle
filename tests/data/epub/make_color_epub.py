# color-image.epub: a chapter with a color picture, for the Colorsoft tests (pages rendered in
# color). Run:
#   python3 tests/data/epub/make_color_epub.py
# The picture (a PNG made here): a red square over its left half, a blue one over its right.
import struct
import zipfile
import zlib


def png(w, h, pixel):
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


PICTURE = png(240, 120, lambda x, y: (220, 30, 30) if x < 120 else (30, 70, 220))

CH1 = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Colour</title></head><body>
<h2>A picture in colour</h2>
<p>The picture below is red on its left and blue on its right.</p>
<p><img src="picture.png" alt="red and blue" style="width: 100%"/></p>
<p>Text after the picture stays black on white.</p>
</body></html>"""

OPF = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">color-image</dc:identifier>
<dc:title>Colour picture</dc:title><dc:language>en</dc:language></metadata>
<manifest><item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
<item id="pic" href="picture.png" media-type="image/png"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="ch1"/></spine></package>"""

NAV = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Nav</title></head><body>
<nav epub:type="toc"><ol><li><a href="ch1.xhtml">Colour</a></li></ol></nav></body></html>"""

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>"""

with zipfile.ZipFile("tests/data/epub/color-image.epub", "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
    for name, data in [("META-INF/container.xml", CONTAINER), ("content.opf", OPF), ("nav.xhtml", NAV),
                       ("ch1.xhtml", CH1), ("picture.png", PICTURE)]:
        z.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)
