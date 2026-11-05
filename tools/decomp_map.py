#!/usr/bin/env python3
"""Decompilation / recompilation status map for the POP Mach-O.

Joins the Ghidra pseudo-C (decompiled/macho/POP.c), the nm symbol table, the port's
native hooks (source/), the import tables and the ObjC selector audit into one JSON,
and writes a self-contained HTML map (decompiled/macho/decomp_map.html, gitignored:
it contains symbol names from the IPA).

Function status ladder (toward a full source decompilation):
  unnamed  -> Ghidra pseudo-C only, FUN_xxxxxxxx
  named    -> pseudo-C + original symbol name (runs as original ARM on the Vita)
  native   -> replaced by C in source/ via macho_hook (recompiled for the Vita)
  matched  -> clean source reconstructed, listed in docs/decomp_status.tsv

docs/decomp_status.tsv (optional, committed): `<hex addr>\t<native|matched>\t<note>` per line.

Usage: python3 -I tools/decomp_map.py [--json out.json]
"""
import json, os, re, struct, subprocess, sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = lambda *a: os.path.join(ROOT, *a)
SRC_FILES = [P('source', f) for f in sorted(os.listdir(P('source'))) if f.endswith(('.c', '.cpp'))]


def macho_sections(path):
    d = open(path, 'rb').read()
    assert struct.unpack_from('<I', d, 0)[0] == 0xFEEDFACE, 'expected a thin 32-bit Mach-O'
    ncmds, off, secs, segs = struct.unpack_from('<I', d, 16)[0], 28, {}, []
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', d, off)
        if cmd == 1:  # LC_SEGMENT
            vm, vs, fo = struct.unpack_from('<III', d, off + 24)
            segs.append((vm, vs, fo))
            for i in range(struct.unpack_from('<I', d, off + 48)[0]):
                so = off + 56 + 68 * i
                secs[d[so:so + 16].rstrip(b'\0').decode()] = struct.unpack_from('<III', d, so + 32)
        off += sz
    return d, secs, segs


def selrefs(d, secs, segs):
    def cstr(va):
        for vm, vs, fo in segs:
            if vm <= va < vm + vs:
                o = va - vm + fo
                return d[o:d.index(b'\0', o)].decode()
    a, s, o = secs['__objc_selrefs']
    return {cstr(struct.unpack_from('<I', d, o + i)[0]) for i in range(0, s, 4)}


JADE = {  # module prefixes with a well-known meaning; the rest are shown by prefix only
    'AI': 'AI scripting', 'SND': 'Sound', 'GFX': 'Effects', 'MATH': 'Math', 'EVE': 'Event tracks',
    'TEX': 'Textures', 'COL': 'Collision', 'OBJ': 'Game objects', 'WOR': 'World', 'ANI': 'Animation',
    'DYN': 'Dynamics', 'GEO': 'Geometry', 'INO': 'Input', 'LIGHT': 'Lights', 'LOA': 'Loader',
    'BIG': 'Bigfile', 'CAM': 'Camera', 'WAY': 'Waypoints', 'ENG': 'Engine', 'GDI': 'Graphics device',
    'MAT': 'Materials', 'PAG': 'Particles', 'STR': 'Text/strings', 'MDF': 'Modifiers',
}


def subsystem(name):
    if name[:2] in ('-[', '+['):
        return 'iOS ObjC layer'
    if re.match(r'_?Mdl[0-9A-F]{8}_', name):
        return 'AI models (Mdl*)'
    ns = re.match(r'(?:\(anonymous namespace\)::)?([A-Za-z_]\w*)::', name)
    tok = ns.group(1) if ns else name.lstrip('_')
    lib = {'gameswf': 'gameswf (Flash UI)', 'gllive': 'GLLive (online)', 'magma': 'magma UI',
           'std': 'libstdc++', '__gnu_cxx': 'libstdc++'}.get(tok)
    if lib:
        return lib
    for pre, lib in (('FT_', 'FreeType'), ('TT_', 'FreeType'), ('T1_', 'FreeType'), ('T42_', 'FreeType'),
                     ('FTC_', 'FreeType'), ('cff_', 'FreeType'), ('tt_', 'FreeType'), ('ps_', 'FreeType'),
                     ('af_', 'FreeType'), ('t1_', 'FreeType'), ('png_', 'libpng'), ('jpeg_', 'libjpeg'),
                     ('jinit_', 'libjpeg'), ('inflate', 'zlib'), ('deflate', 'zlib'), ('Prince_', 'Prince behaviours'),
                     ('Mac', 'Mac/iOS platform layer'), ('MENU_', 'Menus'), ('GLL', 'GLLive (online)'),
                     ('CGLLive', 'GLLive (online)'), ('Menu', 'Menus'), ('MagMenu', 'Menus'),
                     ('MagmaJade', 'magma UI'), ('CMagmaJade', 'magma UI'), ('Enemies', 'Enemies & bosses'),
                     ('Boss', 'Enemies & bosses'), ('SandWave', 'Enemies & bosses'), ('SandGriffin', 'Enemies & bosses'),
                     ('Disturber', 'Enemies & bosses'), ('Combat', 'Enemies & bosses'), ('Tutorial', 'Tutorial'),
                     ('Snd', 'Sound engine (C++)'), ('CMusic', 'Sound engine (C++)'), ('TAdpcm', 'Sound engine (C++)'),
                     ('TMultiTrack', 'Sound engine (C++)'), ('OneShot', 'Sound engine (C++)'), ('CTheme', 'Sound engine (C++)'),
                     ('CASync', 'Sound engine (C++)'), ('TPitch', 'Sound engine (C++)'), ('TAdder', 'Sound engine (C++)'),
                     ('Ambiance', 'Sound engine (C++)'), ('ubi', 'ubi (Ubisoft lib)'), ('VCR', 'VCR (replay)'),
                     ('Cam', 'Camera'), ('ft_', 'FreeType'), ('pfr', 'FreeType'), ('bdf', 'FreeType'), ('pcf', 'FreeType'),
                     ('cid', 'FreeType'), ('psh', 'FreeType'), ('sfnt', 'FreeType'), ('t42', 'FreeType'), ('afm', 'FreeType'),
                     ('gray_', 'FreeType'), ('tcf', 'FreeType'), ('Touch', 'Mac/iOS platform layer')):
        if tok.startswith(pre):
            return lib
    m = re.match(r'([A-Z][A-Z0-9]{1,7})_', tok)
    if m:
        return 'Jade ' + m.group(1) + (' · ' + JADE[m.group(1)] if m.group(1) in JADE else '')
    return 'Other C/C++'


def bar(frac, width=20):
    full = round(frac * width * 8)
    return ('█' * (full // 8) + ('▏▎▍▌▋▊▉'[full % 8 - 1] if full % 8 else '')).ljust(width, '░')


def write_md(data, out):
    """docs/DECOMP_STATUS.md: counts, subsystem names, port hooks and Apple API names only (safe to commit)."""
    F = data['funcs']
    total = sum(f[1] for f in F)
    by = lambda pred: [f for f in F if pred(f)]
    kb = lambda rows: sum(f[1] for f in rows)
    done = lambda f: f[4] in ('native', 'matched')
    ladder = [('Pseudo-C (Ghidra)', by(lambda f: f[4] != 'failed')),
              ('Con nombre original', by(lambda f: f[4] not in ('unnamed', 'failed'))),
              ('Recompilado nativo (C para Vita)', by(done)),
              ('Fuente reconstruida (`docs/decomp_status.tsv`)', by(lambda f: f[4] == 'matched'))]
    subs = {}
    for f in F:
        s = subs.setdefault(f[3], [0, 0, 0])
        s[0] += 1; s[1] += f[1]; s[2] += f[1] if done(f) else 0
    order = sorted(subs.items(), key=lambda kv: -kv[1][1])
    imp = Counter(r[1] for r in data['imports'])
    sel_ok = sum(r[1] for r in data['selectors'])
    L = ['# Estado de decompilación / recompilación', '',
         '<!-- Generado por tools/decomp_map.py (hook pre-commit). No editar a mano. -->', '',
         'Ejecutable iOS `POP` (ARMv6, motor Jade), `__TEXT,__text` `0x%08x`–`0x%08x`: **%d funciones, %d KB**. '
         'La Vita ejecuta el ARM original vía el loader Mach-O; "nativo" = reescrito en C y compilado para la Vita.'
         % (data['text'][0], data['text'][1], len(F), total // 1024), '',
         '| Etapa | Funciones | % bytes | Progreso |', '|---|--:|--:|---|']
    for name, rows in ladder:
        L.append('| %s | %d | %.2f %% | `%s` |' % (name, len(rows), 100 * kb(rows) / total, bar(kb(rows) / total)))
    L += ['', '## Código por subsistema', '', '```mermaid', 'pie showData title KB de código por subsistema']
    for name, (n, b, _) in order[:10]:
        L.append('    "%s" : %d' % (name.replace('"', "'"), b // 1024))
    L.append('    "Resto (%d subsistemas)" : %d' % (len(order) - 10, sum(v[1] for _, v in order[10:]) // 1024))
    L += ['```', '', '| Subsistema | Funcs | KB | Nativo | % |', '|---|--:|--:|---|--:|']
    for name, (n, b, nb) in order:
        L.append('| %s | %d | %d | `%s` | %.1f |' % (name, n, b // 1024, bar(nb / b, 12), 100 * nb / b))
    L += ['', '## Funciones nativas / reconstruidas', '', '| Dirección | Función | Subsistema | Implementación |', '|---|---|---|---|']
    L += ['| `0x%08x` | `%s` | %s | %s |' % (f[0], f[2], f[3], f[5]) for f in by(done)]
    L += ['', '## Imports del loader', '',
          '%d símbolos: **%d** implementados, **%d** stubs, **%d** clases ObjC del despachador, **%d** sin resolver.'
          % (len(data['imports']), imp['native'], imp['stub'], imp['dynamic'], imp['missing']), '',
          '`%s`' % bar(imp['native'] / len(data['imports']), 30), '',
          '<details><summary>Stubs y sin resolver</summary>', '']
    L += ['- `%s` → %s%s' % (r[0], r[2] or '—', ' **(sin resolver)**' if r[1] == 'missing' else '')
          for r in data['imports'] if r[1] in ('stub', 'missing')]
    L += ['', '</details>', '', '## Selectores ObjC a clases del sistema', '',
          '%d selectores: **%d** implementados en `source/objc.c`, **%d** devuelven nil.'
          % (len(data['selectors']), sel_ok, len(data['selectors']) - sel_ok), '',
          '`%s`' % bar(sel_ok / len(data['selectors']), 30), '',
          '<details><summary>Sin implementar</summary>', '',
          ' '.join('`%s`' % r[0] for r in data['selectors'] if not r[1]), '', '</details>', '']
    open(out, 'w').write('\n'.join(L))


def main():
    d, secs, segs = macho_sections(P('game_app', 'POP'))
    t_lo, t_sz, _ = secs['__text']
    t_hi = t_lo + t_sz

    # nm symbols (t/T) -> demangled names
    syms = {}
    for line in open(P('decompiled', 'macho', 'symbols.txt')):
        f = line.split(None, 2)
        if len(f) == 3 and f[1] in 'tT':
            syms.setdefault(int(f[0], 16), f[2].strip())
    by_name = {v: k for k, v in syms.items()}
    addrs = sorted(syms)
    dem = subprocess.run(['c++filt'], input='\n'.join(syms[a] for a in addrs), capture_output=True, text=True).stdout.split('\n')
    demangled = dict(zip(addrs, dem))

    # Ghidra functions inside __text
    funcs, failed, cur = {}, set(), None
    for line in open(P('decompiled', 'macho', 'POP.c'), errors='replace'):
        if line.startswith('// ==== '):
            m = re.match(r'// ==== (.+) @ ([0-9a-f]+) ====', line)
            cur = int(m.group(2), 16)
            if t_lo <= cur < t_hi:
                funcs[cur] = m.group(1)
            else:
                cur = None
        elif cur is not None and line.startswith('// decompile failed'):
            failed.add(cur)

    # native hooks in the port
    src = {f: open(f, errors='replace').read() for f in SRC_FILES}
    native = {}
    for f, s in src.items():
        for m in re.finditer(r'macho_hook\("([^"]+)"\s*,\s*(\w+)', s):
            if m.group(1) in by_name:
                native[by_name[m.group(1)]] = (os.path.basename(f), m.group(2))
        for m in re.finditer(r'macho_hook_addr\(\s*MACHO_SLIDE\s*\+\s*(0x[0-9a-fA-F]+)\s*,\s*(\w+)', s):
            native[int(m.group(1), 16)] = (os.path.basename(f), m.group(2))
    manual = {}
    tsv = P('docs', 'decomp_status.tsv')
    if os.path.exists(tsv):
        for line in open(tsv):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 2 and not line.startswith('#'):
                manual[int(f[0], 16)] = (f[1], f[2] if len(f) > 2 else '')

    starts = sorted(funcs)
    rows, subs = [], Counter()
    for i, a in enumerate(starts):
        size = (starts[i + 1] if i + 1 < len(starts) else t_hi) - a
        name = demangled.get(a) or funcs[a]
        if name == syms.get(a) and name.startswith('_'):
            name = name[1:]  # C symbol: drop the Mach-O underscore
        unnamed = a not in syms and funcs[a].startswith('FUN_')
        st = 'unnamed' if unnamed else 'named'
        note = ''
        if a in native:
            st, note = 'native', '%s: %s' % native[a]
        if a in manual:
            st, note = manual[a]
        if a in failed:
            st = 'failed'
        sub = subsystem(name)
        subs[sub] += 1
        rows.append([a, size, name, sub, st, note])

    # imports: resolved natively / stubbed / missing (falls to the loader's logging thunk)
    imports = [l.split()[-1] for l in open(P('decompiled', 'macho', 'imports.txt')) if l.strip()]
    allsrc = '\n'.join(src.values())
    table = {}
    for m in re.finditer(r'\b[FDV]\(([\w$]+)(?:,\s*&?(\w+))?\)', allsrc):
        table['_' + m.group(1)] = m.group(2) or m.group(1)
    for m in re.finditer(r'\{"(_[\w$]+)",\s*\(uintptr_t\)\s*&?(\w+)', allsrc):
        table[m.group(1)] = m.group(2)

    def stubby(fn):
        # ponytail: "stub" = one-line body or a *fail/noop/ret_obj target; misses multi-line fakes.
        if re.search(r'fail|noop|stub|ret_obj|dummy', fn):
            return True
        return bool(re.search(r'\b%s\([^)]*\)\s*\{[^{}\n]*\}' % re.escape(fn), allsrc))

    imp_rows = []
    for name in imports:
        fn = table.get(name)
        if fn is None:  # macho.c: unknown ObjC classes get a placeholder that objc_msgSend's system dispatcher serves
            st = 'dynamic' if name.startswith('_OBJC_') else 'missing'
        else:
            st = 'stub' if stubby(fn) else 'native'
        imp_rows.append([name, st, fn or ''])

    # ObjC: selectors the game sends to system classes vs. handled in objc.c
    guest = set(re.findall(r'imp +0x[0-9a-f]+ [-+]\[\S+ ([^\]]+)\]', open(P('decompiled', 'macho', 'objc_classes.txt')).read()))
    handled = set(re.findall(r'sel_is\(sel, "([^"]+)"\)', src[P('source', 'objc.c')]))
    host = sorted(selrefs(d, secs, segs) - guest)
    sel_rows = [[s, s in handled] for s in host]

    data = {
        'generated': subprocess.run(['git', '-C', ROOT, 'log', '-1', '--format=%h %cs'], capture_output=True, text=True).stdout.strip(),
        'text': [t_lo, t_hi], 'funcs': rows, 'imports': imp_rows, 'selectors': sel_rows,
    }
    if '--json' in sys.argv:
        json.dump(data, open(sys.argv[sys.argv.index('--json') + 1], 'w'))
    html = open(P('tools', 'decomp_map.html')).read().replace('/*__DATA__*/null', json.dumps(data, separators=(',', ':')))
    write_md(data, P('docs', 'DECOMP_STATUS.md'))
    out = P('decompiled', 'macho', 'decomp_map.html')
    open(out, 'w').write(html)

    c = Counter(r[4] for r in rows)
    assert sum(c.values()) == len(funcs) and all(r[1] > 0 for r in rows), 'function sizes must be positive'
    print('%d functions in __text: %s' % (len(rows), dict(c)))
    print('imports: %s' % dict(Counter(r[1] for r in imp_rows)))
    print('host selectors: %d handled / %d' % (sum(r[1] for r in sel_rows), len(sel_rows)))
    print('->', out)


if __name__ == '__main__':
    main()
