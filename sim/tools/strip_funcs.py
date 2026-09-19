#!/usr/bin/env python3
"""Remove whole function definitions (by name) from a C file: strip_funcs.py file.c Name1 Name2 ..."""
import re, sys
path = sys.argv[1]
names = set(sys.argv[2:])
lines = open(path).read().split('\n')
out = []
i = 0
removed = []
while i < len(lines):
    l = lines[i]
    m = re.match(r'^(static\s+)?(inline\s+)?[\w\s\*]+?\b(\w+)\s*\([^;]*\)\s*$', l)
    if m and m.group(3) in names and i + 1 < len(lines) and lines[i + 1].startswith('{'):
        j = i + 1
        while not lines[j].startswith('}'):
            j += 1
        removed.append(m.group(3))
        i = j + 1
        continue
    out.append(l)
    i += 1
open(path, 'w').write('\n'.join(out))
print(path, 'removed:', ' '.join(removed), '| not found:', ' '.join(sorted(names - set(removed))))
