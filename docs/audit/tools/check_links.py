#!/usr/bin/env python3
"""Controlla i link dei documenti in docs/audit.

- link a file locali: il file deve esistere;
- ancore di riga `#Lx` / `#Lx-Ly`: le righe devono esistere nel file;
- ancore `#b-xx`, `#flusso-n`, `#voci-minori` (esplicite con <a id>): devono
  esistere nel documento di destinazione.

Uso: python3 docs/audit/tools/check_links.py   (dalla radice del repository)
Esce con codice 1 se trova problemi.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.dirname(HERE)
EXPLICIT = re.compile(r'(b-\d+|flusso-\d+|voci-minori)$')

docs = sorted(glob.glob(os.path.join(DOCS, '*.md')))
ids = {os.path.basename(d): set(re.findall(r'<a id="([^"]+)"', open(d, encoding='utf-8').read()))
       for d in docs}
problems, checked = [], 0
for d in docs:
    base = os.path.basename(d)
    text = open(d, encoding='utf-8').read()
    for m in re.finditer(r'\]\(([^)\s]+)\)', text):
        href = m.group(1)
        checked += 1
        if href.startswith(('http:', 'https:', 'mailto:')):
            continue
        path, _, frag = href.partition('#')
        if not path:
            if EXPLICIT.match(frag) and frag not in ids[base]:
                problems.append((base, href, 'ancora mancante'))
            continue
        full = os.path.normpath(os.path.join(os.path.dirname(d), path))
        if not os.path.exists(full):
            problems.append((base, href, 'file mancante'))
            continue
        target = os.path.basename(full)
        if target in ids and frag and EXPLICIT.match(frag) and frag not in ids[target]:
            problems.append((base, href, 'ancora mancante nel file di destinazione'))
        lines = re.fullmatch(r'L(\d+)(?:-L(\d+))?', frag or '')
        if lines and os.path.isfile(full):
            n = sum(1 for _ in open(full, errors='replace'))
            a, b = int(lines.group(1)), int(lines.group(2) or lines.group(1))
            if a < 1 or b > n or a > b:
                problems.append((base, href, f'righe {a}-{b} su {n}'))

print(f'link controllati: {checked}, problemi: {len(problems)}')
for p in problems:
    print('  ', *p)
sys.exit(1 if problems else 0)
