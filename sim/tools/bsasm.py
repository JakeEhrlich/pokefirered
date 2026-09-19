#!/usr/bin/env python3
"""Assemble pokefirered battle scripts (.s using asm/macros/battle_script.inc) into a C bytecode blob.

Pointers inside the blob are encoded as 0x08000000 | offset (ROM-like).
Pointers to C symbols (RAM variables / const tables) are encoded as 0x02000000 | (symId << 12) | byteOffset.
The C runtime decodes them via SimDecodePtr().
"""
import os, re, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CPP_FLAGS = ['-I', os.path.join(ROOT, 'include'), '-DFIRERED', '-DREVISION=0', '-DENGLISH', '-DMODERN=1', '-Wno-trigraphs']

def preprocess(path):
    """Inline .include directives, then run the C preprocessor (assembler-with-cpp mode)."""
    with open(path) as f:
        text = f.read()
    def repl(m):
        inc = os.path.join(ROOT, m.group(1))
        with open(inc) as f:
            return f.read()
    text = re.sub(r'^\s*\.include\s+"([^"]+)"\s*$', repl, text, flags=re.M)
    res = subprocess.run(['clang', '-E', '-x', 'assembler-with-cpp'] + CPP_FLAGS + ['-'],
                         input=text, capture_output=True, text=True, check=True)
    return res.stdout

def strip_comment(line):
    # '@' starts a comment in GAS ARM syntax; '#' lines from cpp are dropped
    out = []
    in_str = False
    for ch in line:
        if ch == '"':
            in_str = not in_str
        if ch == '@' and not in_str:
            break
        out.append(ch)
    return ''.join(out).strip()

OPCHARS = set('+-*/|&<>^~%!=()')

def split_args(s):
    """Split macro args on commas, and on whitespace when the whitespace does not sit inside an
    expression (i.e. neither neighbour is an operator character). Mirrors what GAS does for lines
    such as `setbyte gBattleScripting + 0x1A 1 | 1 << 4`."""
    args, depth, cur = [], 0, ''
    i, n = 0, len(s)
    while i < n:
        ch = s[i]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip()); cur = ''
            i += 1
            continue
        if ch in ' \t' and depth == 0:
            j = i
            while j < n and s[j] in ' \t':
                j += 1
            prev = cur.rstrip()[-1:] if cur.strip() else ''
            nxt = s[j] if j < n else ''
            if nxt == ',' or nxt == '':
                i = j
                continue
            if prev in OPCHARS or nxt in OPCHARS:
                cur += ' '
                i = j
                continue
            # separator
            if cur.strip():
                args.append(cur.strip()); cur = ''
            i = j
            continue
        cur += ch
        i += 1
    if cur.strip():
        args.append(cur.strip())
    return args

class Macro:
    def __init__(self, name, params, body):
        self.name = name
        self.params = params  # list of (name, default or None, required)
        self.body = body      # list of lines

class Assembler:
    def __init__(self):
        self.macros = {}
        self.sets = {}
        self.labels = {}        # name -> offset
        self.globals = []       # names declared with '::' (in order)
        self.items = []         # (kind, size, exprs, line) for pass 2
        self.offset = 0
        self.externs = {}       # name -> id
        self.extern_list = []
        self.label_order = []

    def parse_macros(self, lines):
        i = 0
        out = []
        while i < len(lines):
            line = lines[i]
            m = re.match(r'\.macro\s+(\w+)\s*(.*)$', line)
            if m:
                name = m.group(1)
                params = []
                for p in split_args(m.group(2)) if m.group(2).strip() else []:
                    p = p.strip()
                    req = p.endswith(':req')
                    if req: p = p[:-4]
                    default = None
                    if '=' in p:
                        p, default = p.split('=', 1)
                    params.append((p.strip(), default, req))
                body = []
                i += 1
                while not lines[i].startswith('.endm'):
                    body.append(lines[i]); i += 1
                self.macros[name] = Macro(name, params, body)
            else:
                out.append(line)
            i += 1
        return out

    def expand(self, line, depth=0):
        """Expand a line into primitive directives / labels. Returns list of lines."""
        if depth > 20:
            raise RuntimeError('macro recursion: ' + line)
        m = re.match(r'(\w+)\s*(.*)$', line)
        if not m:
            return [line]
        name, rest = m.group(1), m.group(2)
        if name not in self.macros:
            return [line]
        mac = self.macros[name]
        args = split_args(rest) if rest.strip() else []
        subst = {}
        for idx, (pname, default, req) in enumerate(mac.params):
            if idx < len(args) and args[idx] != '':
                val = args[idx]
            elif default is not None:
                val = default
            elif req:
                raise RuntimeError(f'missing required param {pname} for {name}: {line}')
            else:
                val = ''
            subst[pname] = val
        out = []
        for bl in mac.body:
            s = bl
            for pname, val in subst.items():
                s = s.replace('\\' + pname, val)
            s = s.replace('\\()', '')
            out.extend(self.expand(s, depth + 1))
        return out

    def pass1(self, lines):
        for raw in lines:
            line = strip_comment(raw)
            if not line or line.startswith('#'):
                continue
            # labels (possibly followed by more on the same line — not used here)
            lm = re.match(r'^(\w+)(::?)\s*$', line)
            if lm:
                name, kind = lm.group(1), lm.group(2)
                if name in self.labels:
                    raise RuntimeError('duplicate label ' + name)
                self.labels[name] = self.offset
                self.label_order.append(name)
                if kind == '::':
                    self.globals.append(name)
                continue
            if line.startswith('.'):
                self.directive(line)
                continue
            for ex in self.expand(line):
                ex = ex.strip()
                if not ex:
                    continue
                if ex.startswith('.'):
                    self.directive(ex)
                else:
                    raise RuntimeError('unknown statement: ' + ex + '   (from: ' + line + ')')

    def directive(self, line):
        m = re.match(r'\.(\w+)\s*(.*)$', line)
        d, rest = m.group(1), m.group(2)
        if d == 'set':
            name, val = split_args(rest)
            self.sets[name.strip()] = self.eval(val)
        elif d in ('section', 'text', 'global', 'globl', 'type'):
            pass
        elif d == 'align':
            n = int(rest)
            a = 1 << n
            while self.offset % a:
                self.items.append(('byte', 1, ['0'], line)); self.offset += 1
        elif d == 'space':
            for _ in range(int(self.eval(rest))):
                self.items.append(('byte', 1, ['0'], line)); self.offset += 1
        elif d in ('byte', '2byte', '4byte'):
            size = {'byte': 1, '2byte': 2, '4byte': 4}[d]
            exprs = split_args(rest)
            for e in exprs:
                self.items.append((d, size, [e], line))
                self.offset += size
        else:
            raise RuntimeError('unknown directive: ' + line)

    def eval(self, expr):
        """Evaluate an expression; returns int. Labels resolve to 0x08000000|offset, externs to 0x02000000|id<<12."""
        def sub(m):
            name = m.group(0)
            if name in self.sets:
                return str(self.sets[name])
            if name in self.labels:
                return str(0x08000000 | self.labels[name])
            if re.match(r'^(0x[0-9a-fA-F]+|\d+)$', name):
                return name
            # extern C symbol
            if name not in self.externs:
                self.externs[name] = len(self.extern_list)
                self.extern_list.append(name)
            return str(0x02000000 | (self.externs[name] << 12))
        e = re.sub(r'[A-Za-z_]\w*|0x[0-9a-fA-F]+|\d+', sub, expr)
        try:
            return int(eval(e, {'__builtins__': {}}, {}))
        except Exception as ex:
            raise RuntimeError(f'cannot eval {expr!r} -> {e!r}: {ex}')

    def pass2(self):
        blob = bytearray()
        for kind, size, exprs, line in self.items:
            v = self.eval(exprs[0])
            v &= (1 << (8 * size)) - 1
            blob += v.to_bytes(size, 'little')
        return blob

    def pointer_tables(self):
        """Find global labels whose contents are exclusively .4byte label refs -> emit as C arrays."""
        tables = {}
        # Map offsets to item index
        idx_at = {}
        off = 0
        for i, (kind, size, exprs, line) in enumerate(self.items):
            idx_at[off] = i
            off += size
        order = sorted(self.labels.items(), key=lambda kv: kv[1])
        for n, (name, start) in enumerate(order):
            end = order[n + 1][1] if n + 1 < len(order) else off
            if name not in self.globals:
                continue
            entries = []
            o = start
            ok = end > start
            while o < end:
                i = idx_at.get(o)
                if i is None:
                    ok = False; break
                kind, size, exprs, line = self.items[i]
                e = exprs[0].strip()
                if kind != '4byte' or e not in self.labels:
                    ok = False; break
                entries.append(e)
                o += size
            if ok and entries:
                tables[name] = entries
        return tables

def main():
    if len(sys.argv) < 4:
        print('usage: bsasm.py out.c out.h file.s...'); sys.exit(1)
    out_c, out_h, srcs = sys.argv[1], sys.argv[2], sys.argv[3:]
    asm = Assembler()
    for s in srcs:
        text = preprocess(s)
        lines = [strip_comment(l) for l in text.split('\n')]
        lines = [l for l in lines if l and not l.startswith('#')]
        lines = asm.parse_macros(lines)
        # align between files
        asm.directive('.align 2')
        asm.pass1(lines)
    blob = asm.pass2()
    tables = asm.pointer_tables()

    with open(out_h, 'w') as h:
        h.write('// Generated by sim/tools/bsasm.py -- do not edit\n#ifndef GUARD_BATTLE_SCRIPTS_H\n#define GUARD_BATTLE_SCRIPTS_H\n')
        h.write('#include "global.h"\n')
        h.write('extern const u8 gBattleScriptBlob[%d];\n' % len(blob))
        h.write('#define BATTLE_SCRIPT_BLOB_SIZE %d\n' % len(blob))
        for name in asm.label_order:
            if name in tables:
                continue
            h.write('#define %s (gBattleScriptBlob + %d)\n' % (name, asm.labels[name]))
        for name, entries in tables.items():
            h.write('extern const u8 *const %s[%d];\n' % (name, len(entries)))
        h.write('enum {\n')
        for name in asm.extern_list:
            h.write('    BS_EXTERN_%s,\n' % name)
        h.write('    BS_EXTERN_COUNT\n};\n')
        h.write('extern const char *const gBattleScriptExternNames[BS_EXTERN_COUNT];\n')
        h.write('#endif\n')
    with open(out_c, 'w') as c:
        c.write('// Generated by sim/tools/bsasm.py -- do not edit\n#include "battle_scripts.h"\n')
        c.write('const u8 gBattleScriptBlob[%d] = {\n' % len(blob))
        for i in range(0, len(blob), 16):
            c.write('    ' + ','.join('0x%02x' % b for b in blob[i:i+16]) + ',\n')
        c.write('};\n')
        for name, entries in tables.items():
            c.write('const u8 *const %s[%d] = {\n' % (name, len(entries)))
            for e in entries:
                c.write('    %s,\n' % e)
            c.write('};\n')
        c.write('const char *const gBattleScriptExternNames[BS_EXTERN_COUNT] = {\n')
        for name in asm.extern_list:
            c.write('    "%s",\n' % name)
        c.write('};\n')
    with open(os.path.splitext(out_c)[0] + '_externs.c', 'w') as c:
        c.write('// Generated by sim/tools/bsasm.py -- do not edit\n#include "global.h"\n#include "battle.h"\n#include "battle_message.h"\n#include "battle_scripts.h"\n#include "sim_string_tables.h"\n#include "sim_globals.h"\n')
        c.write('void *SimDecodePtr(u32 v)\n{\n    u8 *base;\n    if (v == 0) return NULL;\n')
        c.write('    if ((v >> 24) == 0x08) return (void *)(gBattleScriptBlob + (v & 0xFFFFFF));\n')
        c.write('    switch ((v >> 12) & 0xFFF)\n    {\n')
        for name in asm.extern_list:
            c.write('    case BS_EXTERN_%s: base = (u8 *)&%s; break;\n' % (name, name))
        c.write('    default: return NULL;\n    }\n    return base + (v & 0xFFF);\n}\n')
    print('blob %d bytes, %d labels, %d tables, %d externs' % (len(blob), len(asm.labels), len(tables), len(asm.extern_list)))
    print('externs:', ' '.join(asm.extern_list))
    if os.environ.get('BSASM_VERIFY_ROM'):
        verify(asm, blob, os.environ['BSASM_VERIFY_ROM'], os.environ['BSASM_VERIFY_MAP'])

def verify(asm, blob, rom_path, map_path):
    """Compare the assembled blob against the real ROM, using the linker map for symbol addresses."""
    rom = open(rom_path, 'rb').read()
    addrs = {}
    for line in open(map_path):
        m = re.match(r'\s+0x([0-9a-f]{8})\s+(\w+)\s*$', line)
        if m:
            addrs.setdefault(m.group(2), int(m.group(1), 16))
    # each source file starts at its own ROM address; find file boundaries via first label of each file
    errors = 0
    checked = 0
    off = 0
    order = sorted(((n, o) for n, o in asm.labels.items() if n in addrs), key=lambda kv: kv[1])
    # map blob offset -> rom address using nearest preceding label
    lab_offs = [o for _, o in order]
    lab_names = [n for n, _ in order]
    import bisect
    def rom_addr(o):
        k = bisect.bisect_right(lab_offs, o) - 1
        name = lab_names[k]
        return addrs[name] + (o - lab_offs[k])
    for kind, size, exprs, line in asm.items:
        v = int.from_bytes(blob[off:off+size], 'little')
        ra = rom_addr(off)
        rv = int.from_bytes(rom[ra - 0x08000000: ra - 0x08000000 + size], 'little')
        if size == 4 and (v >> 24) == 0x08 and v != 0x08000000:
            exp = rom_addr(v & 0xFFFFFF)
        elif size == 4 and (v >> 24) == 0x02:
            name = asm.extern_list[(v >> 12) & 0xFFF]
            exp = addrs[name] + (v & 0xFFF)
        else:
            exp = v
        checked += 1
        if exp != rv:
            errors += 1
            if errors < 20:
                print('MISMATCH at blob+%d (rom %08x): have %x expected(rom) %x  [%s]' % (off, ra, exp, rv, line))
        off += size
    print('verified %d items, %d mismatches' % (checked, errors))

if __name__ == '__main__':
    main()
