#!/usr/bin/env python3
"""Census of inline-asm templates (docs/ASM_CENSUS.md, W4).

usage: asm_census.py [--top N] DIR [DIR...]

Counts `asm`/`__asm__`/`asm volatile`/`asm goto`/`asm_inline` call sites in .c/.h/.inc
files under the given directories, by normalised template: the first string literal
(adjacent literals concatenated) of the call, whitespace collapsed, operand numbers
(%0, %1, %[name]) replaced by %N, `lock;` kept. Prints `count<TAB>template`, most frequent first.
"""
import re
import sys
import collections
import os

CALL = re.compile(r'\b(?:__asm__|__asm|asm_inline|asm)\b\s*(?:volatile|__volatile__|goto|inline|\s)*\(')
MACRO = re.compile(r'[A-Z_][A-Z0-9_]{2,}')
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')


def templates(text):
    for m in CALL.finditer(text):
        i = m.end()
        out = []
        while True:
            while i < len(text) and text[i] in ' \t\r\n':
                i += 1
            sm = STR.match(text, i)
            if not sm:
                im = MACRO.match(text, i)  # a leading macro (LOCK_PREFIX, _ASM_ADD ...) is kept by name
                if not im:
                    break
                out.append(' <' + im.group(0) + '> ')
                i = im.end()
                continue
            out.append(sm.group(1))
            i = sm.end()
        if out:
            yield ''.join(out)
        # a macro-built template is not counted (no leading literal)


def norm(t):
    t = t.replace('\\n', ';').replace('\\t', ' ')
    t = re.sub(r'%\[[A-Za-z_0-9]+\]', '%N', t)
    t = re.sub(r'%[0-9]+', '%N', t)
    t = re.sub(r'%%', '%', t)
    t = re.sub(r'\s+', ' ', t).strip()
    t = re.sub(r'\s*;\s*', '; ', t).strip('; ')
    return t


def main():
    args = sys.argv[1:]
    top = 40
    if args[:1] == ['--top']:
        top = int(args[1])
        args = args[2:]
    cnt = collections.Counter()
    for d in args:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith(('.c', '.h', '.inc')):
                    try:
                        text = open(os.path.join(root, f), errors='replace').read()
                    except OSError:
                        continue
                    for t in templates(text):
                        cnt[norm(t)] += 1
    total = sum(cnt.values())
    print(f'# {total} sites, {len(cnt)} distinct templates')
    for t, n in cnt.most_common(top):
        print(f'{n}\t{t}')


if __name__ == '__main__':
    main()
