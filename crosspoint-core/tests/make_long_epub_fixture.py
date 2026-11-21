#!/usr/bin/env python3
"""Deterministic multi-page EPUB stressing CrossPoint's incremental Section.
No network, no external libraries. Layout is longer than the Q2's first-page
viewport but remains below the ChapterXmlSink safety limits.
"""
import sys
from zipfile import ZipFile, ZIP_STORED, ZIP_DEFLATED

out=sys.argv[1]
container='''<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
<rootfiles><rootfile full-path="OEBPS/book.opf"
media-type="application/oebps-package+xml"/></rootfiles></container>'''
opf='''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:title>Long Section Stress Test</dc:title>
<dc:identifier>crosspod-large-chapter</dc:identifier>
<dc:language>en</dc:language></metadata>
<manifest><item id="section" href="long.xhtml"
media-type="application/xhtml+xml"/></manifest>
<spine><itemref idref="section"/></spine></package>'''
text="A long novel can have many paragraphs with deliberate line wrapping, repeated chapter elements and meaningful page breaks."
body="\n".join(f'<p id="p{i}">Paragraph {i}: '+text*2+'</p>'
               for i in range(130))
chapter=f'''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head>
<title>Long Section</title></head><body>
<h1>Long Section</h1>{body}</body></html>'''
with ZipFile(out,"w") as zip:
    zip.writestr("mimetype","application/epub+zip",compress_type=ZIP_STORED)
    zip.writestr("META-INF/container.xml",container,compress_type=ZIP_DEFLATED)
    zip.writestr("OEBPS/book.opf",opf,compress_type=ZIP_DEFLATED)
    zip.writestr("OEBPS/long.xhtml",chapter,compress_type=ZIP_DEFLATED)
