#!/usr/bin/env python3
"""Integration gate for incremental pages, forward/back and benchmark output."""
import re
import subprocess
import sys

reader, book = sys.argv[1:]
def run(command, seconds=90):
    result=subprocess.run([reader,command,book],capture_output=True,
                          text=True,timeout=seconds)
    assert result.returncode==0, (command,result.stdout,result.stderr)
    return result.stdout

opening=run("--validate")
assert "CrossPod original Section:" in opening, opening
assert "(incremental)" in opening, "First open must not paginate whole chapter"

walking=run("--walk",120)
match=re.search(r"CrossPod walked (\d+) pages forward and (\d+) backward",
                walking)
assert match, walking
forward,backward=map(int,match.groups())
assert forward>=10, f"Long chapter yielded only {forward} page turns"
assert forward==backward, (forward,backward)

stats=run("--bench",120)
match=re.search(r"open_ms=(\d+) sample_pages=(\d+) walk_ms=(\d+) maxrss_kb=(\d+)",
                stats)
assert match, stats
start_ms,pages,walk_ms,rss=map(int,match.groups())
assert pages>=10 and rss>0
print(f"Original Section incremental: PASS {forward} pages, "
      f"startup={start_ms}ms, sample={pages}, walk={walk_ms}ms, "
      f"peak RSS={rss}KB")
