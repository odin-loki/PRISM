#!/usr/bin/env python3
"""leanparse.py - a light, regex-level Lean 4 declaration splitter shared by build/verify/search.

It is NOT a Lean parser.  It splits a .lean file into top-level commands (a command starts on a line whose first
column is non-blank once comments/strings are masked), recognises declaration commands, resolves `namespace`/`section`
/`end` so a full name can be built, and separates signature from body at the first top-level `:=` / `where` / `|`.
Known limits are listed in data/lean-db/SCHEMA.md (no macro expansion, `variable`s are not added to signatures, ...).
"""
import hashlib
import re

# ---- masking -----------------------------------------------------------------------------------------------------
_TOK = re.compile(r'''
    (?P<str>"(?:\\.|[^"\\])*")
  | (?P<chr>(?<![\w'.])'(?:\\(?:x[0-9a-fA-F]{2}|u[0-9a-fA-F]{4}|.)|[^\\'\n])')
  | (?P<lc>--[^\n]*)
  | (?P<bo>/-)
  | (?P<bc>-/)
''', re.X | re.S)


def mask(text):
    """Return (masked, docs). masked has comments and string contents replaced by spaces (newlines kept, same length).
    docs: list of (start, end, body) for `/-- ... -/` doc comments (end is index after `-/`)."""
    out = []
    docs = []
    pos = 0
    depth = 0
    cstart = 0
    n = len(text)
    is_doc = False
    for m in _TOK.finditer(text):
        k = m.lastgroup
        s, e = m.span()
        if depth > 0:
            if k == 'bo':
                depth += 1
            elif k == 'bc':
                depth -= 1
                if depth == 0:
                    seg = text[cstart:e]
                    out.append(re.sub(r'[^\n]', ' ', seg))
                    if is_doc:
                        docs.append((cstart, e, text[cstart + 3:e - 2].strip()))
                    pos = e
            continue
        if k == 'bo':
            out.append(text[pos:s])
            depth = 1
            cstart = s
            is_doc = text.startswith('/--', s) and not text.startswith('/--/', s)
            if text.startswith('/-/', s):  # `/-/` is not a comment opener we can close: treat as plain text
                depth = 0
                out.append(text[s:e])
                pos = e
                continue
            pos = s
            continue
        if k == 'bc':
            continue
        if k == 'lc':
            out.append(text[pos:s])
            out.append(' ' * (e - s))
            pos = e
        elif k in ('str', 'chr'):
            out.append(text[pos:s])
            out.append(re.sub(r'[^\n]', ' ', text[s:e]) if k == 'str' else ' ' * (e - s))
            pos = e
    if depth > 0:  # unterminated comment: blank the rest
        out.append(re.sub(r'[^\n]', ' ', text[cstart:]))
        pos = n
    out.append(text[pos:])
    return ''.join(out), docs


# ---- command splitting -------------------------------------------------------------------------------------------
_STARTS = re.compile(
    r'^(?:@\[|#|(?:theorem|lemma|def|instance|abbrev|structure|class|inductive|coinductive|axiom|opaque|example|'
    r'irreducible_def|namespace|section|end|open|variable|universe|set_option|attribute|macro_rules|macro|syntax|'
    r'elab_rules|elab|notation|infixl|infixr|infix|prefix|postfix|local|scoped|private|protected|noncomputable|'
    r'unsafe|partial|nonrec|public|meta|initialize|builtin_initialize|import|module|prelude|mutual|'
    r'export|run_cmd|declare_\w+|register_\w+|add_decl_doc|alias|simproc|builtin_simproc|dsimproc|'
    r'builtin_dsimproc|compile_inductive|compile_def|library_note|assert_not_exists|assert_not_imported|'
    r'suppress_compilation|syntax_cat|extend_docs|unseal|seal|add_aesop_rules|bv_normalize_proc|open_locale|'
    r'derive_\w+|reserve|recommended_spelling|grind_pattern|deriving\s+instance|instance_\w+)\b)')

MODS = r'(?:private|protected|noncomputable|unsafe|partial|nonrec|public|meta|local|scoped)'
KW = (r'(?:theorem|lemma|def|instance|abbrev|structure|class\s+inductive|class\s+abbrev|class|inductive|coinductive|'
      r'axiom|opaque|example|irreducible_def|macro_rules|syntax|macro|elab|notation|infixl|infixr|infix|prefix|postfix)')
DECL_KINDS = {'theorem', 'lemma', 'def', 'instance', 'abbrev', 'structure', 'class', 'inductive', 'coinductive', 'axiom',
              'opaque', 'example', 'irreducible_def', 'class inductive', 'class abbrev'}
_KWRE = re.compile(r'(?:' + MODS + r'\s+)*(' + KW + r')\b')


def split_commands(masked):
    """Yield (start, end) char spans of top-level commands in the masked text."""
    spans = []
    starts = []
    pos = 0
    for line in masked.split('\n'):
        if line and _STARTS.match(line):
            starts.append(pos)
        pos += len(line) + 1
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else len(masked)
        spans.append((s, e))
    # merge standalone attribute commands (`@[simp]` alone on its line) into the next command
    merged = []
    carry = None
    for s, e in spans:
        txt = masked[s:e].strip()
        if txt.startswith('@[') and _balanced_attr_only(txt):
            carry = s if carry is None else carry
            continue
        merged.append((carry if carry is not None else s, e))
        carry = None
    return merged


def _balanced_attr_only(t):
    depth = 0
    for i, ch in enumerate(t):
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
            if depth == 0:
                return t[i + 1:].strip() == ''
    return False


def _read_attrs(masked, orig, s, e):
    """Parse leading `@[...]` blocks. Return (attrs list, index after)."""
    attrs = []
    i = s
    while True:
        while i < e and masked[i].isspace():
            i += 1
        if masked.startswith('@[', i):
            depth = 0
            j = i + 1
            while j < e:
                if masked[j] == '[':
                    depth += 1
                elif masked[j] == ']':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            attrs.append(orig[i + 2:j].strip())
            i = j + 1
        else:
            return attrs, i


_NAME = re.compile(r'[^\s(){}\[\]⦃⦄:,]+(?:\.\{[^}]*\})?')
_OPEN_BR = '([{⦃'
_CLOSE_BR = ')]}⦄'


def _split_sig_body(masked, orig, a, e, kind):
    """Find first top-level `:=` / `where` / pattern-matching `|` in masked[a:e]; return (sig_end, body_start)."""
    depth = 0
    i = a
    line_start = True
    while i < e:
        ch = masked[i]
        if ch in _OPEN_BR:
            depth += 1
        elif ch in _CLOSE_BR:
            depth = max(0, depth - 1)
        elif depth == 0:
            if ch == ':' and masked.startswith(':=', i):
                return i, i + 2
            if masked.startswith('where', i) and (i == 0 or not (masked[i - 1].isalnum() or masked[i - 1] in "_'.")) \
                    and (i + 5 >= e or not (masked[i + 5].isalnum() or masked[i + 5] in "_'.")):
                return i, i + 5
            if ch == '|' and line_start:
                return i, i
        if ch == '\n':
            line_start = True
        elif not ch.isspace():
            line_start = False
        i += 1
    return e, e


class Decl:
    __slots__ = ('kind', 'name', 'full_name', 'namespace', 'line', 'end_line', 'signature', 'body', 'docstring',
                 'attrs', 'modifiers', 'start', 'end', 'stmt_end', 'body_start')


def parse_decls(text, want_all_kinds=False):
    """Parse declarations out of Lean source text. Returns list[Decl] in file order."""
    masked, docs = mask(text)
    # line start offsets for line numbers
    nl = [m.start() for m in re.finditer('\n', text)]
    import bisect

    def lineno(off):
        return bisect.bisect_right(nl, off - 1) + 1 if off > 0 else 1
    docs_by_end = {}
    for s, e, body in docs:
        docs_by_end[e] = (s, body)
    ends_sorted = sorted(docs_by_end)
    stack = []  # (type, label, parts)
    res = []
    for s, e in split_commands(masked):
        e = max(s + 1, len(masked[:e].rstrip()))  # drop trailing comments/blank lines (they belong to the next command)
        head = masked[s:e]
        hs = head.lstrip()
        if hs.startswith('namespace'):
            m = re.match(r'namespace\s+(\S+)', hs)
            if m:
                parts = m.group(1).replace('«', '').replace('»', '').split('.')
                stack.append(('ns', m.group(1), parts))
            continue
        if hs.startswith('section'):
            m = re.match(r'section(?:\s+(\S+))?', hs)
            stack.append(('sec', m.group(1) if m else None, []))
            continue
        if hs.startswith('mutual'):
            stack.append(('mut', None, []))
            continue
        if re.match(r'end\b', hs):
            m = re.match(r'end(?:\s+(\S+))?', hs)
            lab = m.group(1) if m else None
            if lab:
                for k in range(len(stack) - 1, -1, -1):
                    if stack[k][1] == lab:
                        del stack[k:]
                        break
            elif stack:
                stack.pop()
            continue
        attrs, i = _read_attrs(masked, text, s, e)
        m = _KWRE.match(masked, i)
        if not m:
            continue
        kw = re.sub(r'\s+', ' ', m.group(1))
        if kw not in DECL_KINDS and not want_all_kinds:
            continue
        mods = [x for x in re.findall(MODS, masked[i:m.start(1)])]
        # name
        j = m.end()
        while j < e and masked[j].isspace():
            j += 1
        name = ''
        if kw in ('example',):
            pass
        else:
            # instance: skip optional priority `(priority := ..)`
            if kw == 'instance':
                while masked.startswith('(', j) and 'priority' in masked[j:j + 14]:
                    k = masked.find(')', j)
                    j = k + 1 if k > 0 else j
                    while j < e and masked[j].isspace():
                        j += 1
            nm = _NAME.match(masked, j)
            if nm and not (kw == 'instance' and masked[j] in ':'):
                name = text[nm.start():nm.end()]
                name = re.sub(r'\.\{[^}]*\}$', '', name)
                j = nm.end()
        sig_end, body_start = _split_sig_body(masked, text, j, e, kw)
        # trim trailing whitespace
        # docstring directly before s (allow attributes handled since s covers them)
        doc = ''
        k = bisect.bisect_right(ends_sorted, s) - 1
        if k >= 0:
            de = ends_sorted[k]
            if not text[de:s].strip():
                doc = docs_by_end[de][1]
        ns_parts = []
        for t, lab, parts in stack:
            ns_parts.extend(parts)
        namespace = '.'.join(ns_parts)
        if name.startswith('_root_.'):
            full = name[7:]
        elif name:
            full = (namespace + '.' + name) if namespace else name
        else:
            full = ''
        d = Decl()
        d.kind = 'class' if kw == 'class' else kw
        d.name = name
        d.full_name = full
        d.namespace = namespace
        d.start = s
        d.end = e
        d.line = lineno(s + (len(head) - len(hs)))
        d.end_line = lineno(max(s, e - 1))
        d.signature = text[i:sig_end].strip()
        d.body = text[body_start:e].strip() if body_start < e else ''
        d.docstring = doc
        d.attrs = '; '.join(attrs)
        d.modifiers = ' '.join(mods)
        d.stmt_end = sig_end
        d.body_start = body_start
        res.append(d)
    return res


# ---- normalisation / hashing -------------------------------------------------------------------------------------
_TRAIL = re.compile(r'\s*:=\s*(?:by\b)?\s*(?:sorry\b)?\s*$')
_WS = re.compile(r'\s+')


def norm_statement(stmt, name=None):
    """Whitespace- and comment-normalised statement with the declaration name anonymised and any trailing
    `:= by sorry` removed. Used for norm_hash (dedup / contamination)."""
    if not stmt:
        return ''
    t = re.sub(r'/-.*?-/', ' ', stmt, flags=re.S)
    t = re.sub(r'--[^\n]*', ' ', t)
    t = _TRAIL.sub('', t.strip())
    t = re.sub(r'^((?:@\[[^\]]*\]\s*)*(?:' + MODS + r'\s+)*)(theorem|lemma|def|instance|abbrev|example)\s+(?!:|\(|\{|\[)\S+',
               r'\2 _', t)
    t = _WS.sub(' ', t).strip()
    return t


def norm_hash(stmt, name=None):
    n = norm_statement(stmt, name)
    return hashlib.sha256(n.encode('utf-8')).hexdigest() if n else None


def tokens(text):
    """Cheap Lean-ish tokeniser for shingling (identifiers incl. dots, numbers, single symbols)."""
    return re.findall(r"[A-Za-z_Ͱ-Ͽᴀ-ᶿ][\w'.!?₀-ₜ]*|\d+|[^\s\w]", text)


_IMPORT = re.compile(r'^\s*(?:public\s+|meta\s+)?import\s+(?:all\s+)?(\S+)', re.M)


def imports_of(text):
    head = text.split('\n\n/-', 1)[0] if False else text
    out = []
    for line in text.split('\n'):
        s = line.strip()
        if s.startswith('import ') or s.startswith('public import ') or s.startswith('meta import '):
            m = _IMPORT.match(line)
            if m:
                out.append(m.group(1))
        elif s and not s.startswith('--') and not s.startswith('/-') and not s.startswith('*') and \
                not s.startswith('module') and not s.startswith('prelude') and not s.startswith('-/') and \
                not s.startswith('import') and s != '':
            if out or s.startswith(('namespace', 'open', 'theorem', 'def', 'set_option', 'universe', 'variable')):
                break
    return out


if __name__ == '__main__':
    import sys
    for f in sys.argv[1:]:
        for d in parse_decls(open(f, encoding='utf-8').read()):
            print(d.line, d.kind, d.full_name, '|', d.signature[:80].replace('\n', ' '), '|', d.body[:40].replace('\n', ' '))
