#!/usr/bin/env python3
"""Verify the REAL CrossPoint EPUB 3 nav and EPUB 2 NCX TOC adapters."""
import subprocess
import sys
import tempfile
from pathlib import Path
from zipfile import ZipFile

reader, book = sys.argv[1:3]

def inspect(path):
    result = subprocess.run([reader, "--inspect-toc", str(path)],
                            capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    assert "CrossPod TOC entries: 1" in result.stdout, result.stdout
    assert "toc=0 spine=0" in result.stdout, result.stdout
    assert "anchor=chapter-start" in result.stdout, result.stdout
    return result.stdout

first = inspect(book)
assert "title=Illustrated chapter" in first, first

# Force the original EPUB 2 parser by removing EPUB 3's nav resource.
with tempfile.TemporaryDirectory(prefix="crosspod-ncx-test-") as td:
    target = Path(td) / "ncx.epub"
    with ZipFile(book) as source, ZipFile(target, "w") as output:
        for item in source.infolist():
            if item.filename != "OEBPS/nav.xhtml":
                output.writestr(item, source.read(item.filename))
    second = inspect(target)
    assert "title=Illustrated chapter NCX" in second, second

print("CrossPoint original EPUB3 NAV and EPUB2 NCX TOC parsing: PASS")
