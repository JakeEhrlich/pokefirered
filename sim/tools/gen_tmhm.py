#!/usr/bin/env python3
import re, os, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
src = open(os.path.join(root, 'src/data/party_menu.h')).read()
m = re.search(r'static const u16 sTMHMMoves\[\] =\s*\{.*?\};', src, re.S)
open(sys.argv[1], 'w').write('// Generated from src/data/party_menu.h by sim/tools/gen_tmhm.py -- do not edit\n' + m.group(0) + '\n')
