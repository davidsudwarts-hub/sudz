#!/usr/bin/env python3
"""Make the bundled page pure ASCII so no web view can misread its encoding.
Non-ASCII characters become \\uXXXX inside <script> and &#xXXXX; elsewhere."""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8').read()
def js(m):
    out = []
    for ch in m.group(0):
        o = ord(ch)
        if o < 128: out.append(ch)
        elif o <= 0xFFFF: out.append('\\u%04x' % o)
        else:
            o -= 0x10000; out.append('\\u%04x\\u%04x' % (0xD800 + (o >> 10), 0xDC00 + (o & 0x3FF)))
    return ''.join(out)
def css(m):
    return ''.join(ch if ord(ch) < 128 else '\\%x ' % ord(ch) for ch in m.group(0))
parts = re.split(r'(<script\b[^>]*>.*?</script>|<style\b[^>]*>.*?</style>)', s, flags=re.S)
res = []
for p in parts:
    if p.startswith('<script'): res.append(re.sub(r'[\s\S]+', js, p))
    elif p.startswith('<style'): res.append(re.sub(r'[\s\S]+', css, p))
    else: res.append(''.join(ch if ord(ch) < 128 else '&#x%x;' % ord(ch) for ch in p))
out = ''.join(res)
assert all(ord(c) < 128 for c in out)
open(dst, 'w', encoding='ascii').write(out)
