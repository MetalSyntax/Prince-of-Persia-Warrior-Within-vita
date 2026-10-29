#!/usr/bin/env python3
"""Genera source/cxx_imports.c: mapea los imports C++ del Mach-O a libstdc++ de VitaSDK.

El ABI de libstdc++ (std::string COW, iostreams) es binariamente compatible.
Única diferencia de mangling: size_t es unsigned long en iOS ('m') y
unsigned int en ARM EABI ('j').

Uso: python3 -I tools/gen_cxx_imports.py [salida.c]
"""
import os
import subprocess
import sys

out = sys.argv[1] if len(sys.argv) > 1 else 'source/cxx_imports.c'
sdk = os.environ.get('VITASDK', '/home/metalsyntax/vitasdk')

with open('decompiled/macho/imports.txt') as f:
    undef = [line.strip() for line in f if line.strip()]

ios = sorted({u[1:] for u in undef if u.startswith('__Z')})

vita = {}
nm = subprocess.run([f'{sdk}/bin/arm-vita-eabi-nm', '-g', '--defined-only', f'{sdk}/arm-vita-eabi/lib/libstdc++.a'],
                    capture_output=True, text=True, check=True).stdout
for line in nm.splitlines():
    p = line.split()
    if len(p) == 3:
        vita[p[2]] = p[1]

by_len = {}
for v in vita:
    by_len.setdefault(len(v), []).append(v)

rows, unmapped = [], []
for s in ios:
    cand = [s] if s in vita else [v for v in by_len.get(len(s), [])
                                    if all(a == b or (a == 'm' and b == 'j') for a, b in zip(s, v))]
    if len(cand) != 1:
        unmapped.append(s)
        continue
    rows.append((s, cand[0], vita[cand[0]] in 'VDBRvdbr'))

with open(out, 'w') as f:
    f.write('// Generado por tools/gen_cxx_imports.py: imports C++ del juego -> libstdc++ de VitaSDK (no editar).\n')
    f.write(f'// Sin equivalente: {", ".join(unmapped) or "ninguno"}\n#include "imports.h"\n\n')
    for _s, v, _d in rows:
        f.write(f'extern char {v}[];\n')
    f.write('\nconst import_entry cxx_imports[] = {\n')
    for s, v, d in rows:
        f.write(f'    {{"_{s}", (uintptr_t){v}, {int(d)}}},\n')
    f.write('};\nconst int cxx_imports_count = sizeof(cxx_imports) / sizeof(cxx_imports[0]);\n')

print(f'{len(rows)} mapeados, sin equivalente: {unmapped} -> {out}')
