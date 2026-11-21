#!/usr/bin/env python3
"""EPUB fixture with Chinese Simplified, Chinese Traditional and Japanese text."""
import sys
from zipfile import ZipFile, ZIP_DEFLATED, ZIP_STORED

container = '''<?xml version="1.0" encoding="utf-8"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
<rootfiles><rootfile full-path="OEBPS/content.opf"
media-type="application/oebps-package+xml"/></rootfiles></container>'''
opf = '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier>crosspod-cjk-regression</dc:identifier>
<dc:title>中文・日本語 CJK reader sample</dc:title>
<dc:language>mul</dc:language></metadata>
<manifest><item id="chapter" href="chapter.xhtml"
media-type="application/xhtml+xml"/></manifest>
<spine><itemref idref="chapter"/></spine></package>'''
chapter = '''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head>
<title>中文・日本語</title></head><body>
<h1>中文阅读测试</h1>
<p>中文简体测试，欢迎阅读。今天我们一起看书，天气很好。</p>
<p>繁體中文：歡迎閱讀，今天是美好的一天。</p>
<h2>日本語の読書</h2>
<p>日本語の本を読みます。ひらがな、カタカナ、漢字を表示します。</p>
<p>東京と京都で本を読みます。こんにちは、ありがとう。</p>
</body></html>'''
with ZipFile(sys.argv[1], "w") as z:
    z.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
    z.writestr("META-INF/container.xml", container, compress_type=ZIP_DEFLATED)
    z.writestr("OEBPS/content.opf", opf, compress_type=ZIP_DEFLATED)
    z.writestr("OEBPS/chapter.xhtml", chapter, compress_type=ZIP_DEFLATED)
