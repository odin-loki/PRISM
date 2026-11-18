#!/usr/bin/env python3
"""build_lean_db.py [--root D] [--out FILE] [--only a,b] [--procs N]

Builds ONE sqlite file (default /mnt/d/ParanoidBSD/lean-db/lean_db.sqlite) from the raw Lean-DB downloads
described by tools/lean_db/sources.json + data/lean-db/manifest.tsv.  Idempotent: builds <out>.new then renames it
over <out> atomically.  Never modifies the raw downloads.  Schema: data/lean-db/SCHEMA.md.

Needs python3 with pyarrow + ijson (pip --target <root>/pylib; PYTHONPATH is extended automatically).
Runs at nice 19 with at most --procs (default 5) worker processes.
"""
import argparse, ast, bisect, csv, glob, hashlib, io, json, os, re, sqlite3, subprocess, sys, tarfile, time, zlib
from datetime import datetime, timezone
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import leanparse as LP

SCHEMA_VERSION = '1.0'
ap = argparse.ArgumentParser()
ap.add_argument('--root', default='/mnt/d/ParanoidBSD/lean-db')
ap.add_argument('--out', default=None)
ap.add_argument('--only', default='')
ap.add_argument('--procs', type=int, default=5)
ap.add_argument('--limit-rows', type=int, default=0, help='debug: cap rows per dataset')
A = ap.parse_args() if __name__ == '__main__' else ap.parse_args([])
ROOT = A.root
sys.path.insert(0, os.path.join(ROOT, 'pylib'))
OUT = A.out or os.path.join(ROOT, 'lean_db.sqlite')
TMP = os.path.join(ROOT, 'tmp'); os.makedirs(TMP, exist_ok=True)
os.environ['TMPDIR'] = TMP; os.environ['SQLITE_TMPDIR'] = TMP
try:
    os.nice(19)
except Exception:
    pass
LIM = A.limit_rows


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def sha(s):
    return hashlib.sha256(s.encode('utf-8', 'replace')).hexdigest()


# ------------------------------------------------------------------------------------------------ schema
DDL = r"""
PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE sources(
  name TEXT PRIMARY KEY, kind TEXT NOT NULL, use TEXT NOT NULL CHECK(use IN ('library','data','eval')),
  license TEXT, url TEXT, record TEXT, revision TEXT, fetched_at TEXT, note TEXT,
  provenance TEXT,                  -- 'confirmed' | 'unverified' | NULL
  manifest_files INTEGER, manifest_bytes INTEGER,   -- what the downloader recorded
  files_ingested INTEGER, bytes_ingested INTEGER,   -- what this build actually read
  n_decls INTEGER DEFAULT 0, n_problems INTEGER DEFAULT 0, n_traces INTEGER DEFAULT 0, n_theorems INTEGER DEFAULT 0);
CREATE TABLE files(
  id INTEGER PRIMARY KEY, source TEXT NOT NULL REFERENCES sources(name), path TEXT NOT NULL, bytes INTEGER,
  sha256 TEXT, rows INTEGER DEFAULT 0, row_kind TEXT,        -- decls|problems|traces|theorems|none
  status TEXT NOT NULL DEFAULT 'ingested',                   -- ingested | skipped
  note TEXT, UNIQUE(source,path));
CREATE TABLE headers(id INTEGER PRIMARY KEY, sha256 TEXT UNIQUE, text TEXT NOT NULL);
CREATE TABLE module_imports(source TEXT NOT NULL, module TEXT NOT NULL, file TEXT, imports TEXT, PRIMARY KEY(source,module));
CREATE TABLE decls(
  id INTEGER PRIMARY KEY, source TEXT NOT NULL REFERENCES sources(name), file_id INTEGER REFERENCES files(id),
  module TEXT, file TEXT NOT NULL, line INTEGER, end_line INTEGER,
  kind TEXT NOT NULL, name TEXT, full_name TEXT, namespace TEXT, area TEXT,
  signature TEXT NOT NULL, body TEXT, docstring TEXT, attrs TEXT, modifiers TEXT, extra TEXT,
  norm_hash TEXT, eval_overlap INTEGER NOT NULL DEFAULT 0, searchable INTEGER NOT NULL DEFAULT 1);
CREATE TABLE problems(
  id INTEGER PRIMARY KEY, source TEXT NOT NULL REFERENCES sources(name), use TEXT NOT NULL, file_id INTEGER REFERENCES files(id),
  orig_id TEXT, split TEXT, header_id INTEGER REFERENCES headers(id),
  formal_statement TEXT NOT NULL, formal_proof TEXT, proof_form TEXT,
  informal_statement TEXT, informal_proof TEXT,
  lean_version TEXT, mathlib_version TEXT, verified INTEGER, verified_note TEXT,
  extra TEXT, norm_hash TEXT, eval_overlap INTEGER NOT NULL DEFAULT 0);
CREATE TABLE trace_theorems(
  id INTEGER PRIMARY KEY, source TEXT NOT NULL REFERENCES sources(name), file_id INTEGER REFERENCES files(id),
  repo TEXT, commit_sha TEXT, file TEXT NOT NULL, full_name TEXT, start_line INTEGER, end_line INTEGER,
  statement TEXT, n_tactics INTEGER NOT NULL DEFAULT 0, split_random TEXT, split_novel TEXT,
  norm_hash TEXT, eval_overlap INTEGER NOT NULL DEFAULT 0);
CREATE TABLE traces(
  id INTEGER PRIMARY KEY, theorem_id INTEGER NOT NULL REFERENCES trace_theorems(id), source TEXT NOT NULL,
  idx INTEGER NOT NULL, tactic TEXT NOT NULL, state_before TEXT, state_after TEXT, n_premises INTEGER NOT NULL DEFAULT 0,
  norm_hash TEXT);
CREATE TABLE trace_premises(
  trace_id INTEGER NOT NULL REFERENCES traces(id), full_name TEXT NOT NULL, def_path TEXT, def_line INTEGER,
  decl_id INTEGER REFERENCES decls(id),      -- same-benchmark corpus decl (full_name + def_path), NULL if unresolved
  lib_decl_id INTEGER REFERENCES decls(id)); -- library-snapshot decl (by full_name), NULL if unresolved
"""
INDEXES = r"""
CREATE INDEX decls_src ON decls(source); CREATE INDEX decls_fn ON decls(full_name); CREATE INDEX decls_nh ON decls(norm_hash);
CREATE INDEX decls_mod ON decls(source,module);
CREATE INDEX problems_src ON problems(source); CREATE INDEX problems_nh ON problems(norm_hash);
CREATE INDEX trace_thm_src ON trace_theorems(source,full_name); CREATE INDEX trace_thm_nh ON trace_theorems(norm_hash);
CREATE INDEX traces_thm ON traces(theorem_id); CREATE INDEX traces_src ON traces(source);
CREATE INDEX trace_prem_tr ON trace_premises(trace_id); CREATE INDEX trace_prem_fn ON trace_premises(full_name);
CREATE INDEX trace_prem_decl ON trace_premises(decl_id);
"""
VIEWS = r"""
CREATE VIEW eval_items AS
  SELECT p.id, p.source, p.orig_id, p.split, h.text AS header, p.formal_statement, p.formal_proof, p.informal_statement,
         p.informal_proof, p.lean_version, p.verified, p.norm_hash
  FROM problems p LEFT JOIN headers h ON h.id=p.header_id WHERE p.use='eval';
CREATE VIEW data_problems AS SELECT * FROM problems WHERE use='data';
"""


def conn_new(path):
    if os.path.exists(path):
        os.remove(path)
    c = sqlite3.connect(path)
    c.executescript(DDL)
    return c


# ------------------------------------------------------------------------------------------------ helpers
_SORRY_ONLY = re.compile(r'^(?:by\s+)?sorry$')
_PREAMBLE = ('import', 'set_option', 'open', 'universe', 'prelude', 'module')


def ident_split(s):
    """camelCase / dotted / underscore split of identifiers for the search index (lowercased words)."""
    out = []
    for w in re.findall(r"[A-Za-z][A-Za-z0-9_'.]*", s):
        for p in re.split(r'[._]+', w):
            out.extend(x.lower() for x in re.findall(r'[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+', p))
    return ' '.join(out)


def extract_file(text):
    """Split a whole Lean file (as found in the datasets) into preamble, pre_code, statement, proof and full_decl for
    the LAST theorem/lemma (else last declaration). Returns dict (ok False if no declaration was found)."""
    masked, _docs = LP.mask(text)
    cmds = LP.split_commands(masked)
    pe = 0
    for s, e in cmds:
        w = masked[s:e].split(None, 1)[0] if masked[s:e].strip() else ''
        if w in _PREAMBLE:
            pe = e
        else:
            break
    decls = LP.parse_decls(text)
    thm = [d for d in decls if d.kind in ('theorem', 'lemma')]
    d = (thm or decls or [None])[-1]
    pre = text[:pe].strip()
    if d is None:
        return dict(ok=False, preamble=pre, pre_code=None, statement=text[pe:].strip(), proof=None, full_decl=None)
    mid = text[pe:d.start]
    mm, _ = LP.mask(mid)
    pre_code = mid.strip() if mm.strip() else None
    stmt = text[d.start:d.stmt_end].strip()
    proof = text[d.body_start:d.end].strip() if d.body_start < d.end else ''
    if _SORRY_ONLY.match(proof or 'sorry'):
        proof = None
    return dict(ok=True, preamble=pre, pre_code=pre_code, statement=stmt, proof=proof, full_decl=text[d.start:d.end].strip(),
                name=d.full_name, kind=d.kind)


def _w_extract(args):
    key, text, mode = args
    try:
        r = extract_file(text)
    except Exception as ex:  # never lose a row
        r = dict(ok=False, preamble='', pre_code=None, statement=text, proof=None, full_decl=None, err=repr(ex))
    r['nh'] = LP.norm_hash(r['statement']) if r.get('statement') else None
    return key, r


def parse_lean_file(path_rel_root):
    """worker: parse a library .lean file -> (path, sha, bytes, rows, imports) """
    root, src, rel, area, modname = path_rel_root
    p = os.path.join(root, rel)
    try:
        raw = open(p, 'rb').read()
    except Exception as ex:
        return (rel, None, 0, [], [], repr(ex))
    h = hashlib.sha256(raw).hexdigest()
    text = raw.decode('utf-8', 'replace')
    rows = []
    try:
        for d in LP.parse_decls(text):
            sig = d.signature
            rows.append((modname, rel, d.line, d.end_line, d.kind, d.name, d.full_name, d.namespace, area, sig,
                         d.body, d.docstring, d.attrs, d.modifiers, LP.norm_hash(d.attrs and ('@[' + d.attrs + '] ' + sig) or sig)))
        imps = LP.imports_of(text)
    except Exception as ex:
        return (rel, h, len(raw), [], [], repr(ex))
    return (rel, h, len(raw), rows, imps, None)


TEST_DIRS = {'tests', 'test', 'MathlibTest', 'BatteriesTest', 'Test', 'benchmarks', 'testing'}
DOC_DIRS = {'doc', 'docs', 'Archive', 'Counterexamples', 'scripts', 'script', 'tools'}


def area_of(rel):
    parts = rel.split('/')
    if any(p in TEST_DIRS for p in parts[:3]) or 'tests' in parts:
        return 'test'
    if parts[0] in DOC_DIRS:
        return 'doc'
    return 'src'


def module_of(src, rel):
    r = rel[:-5] if rel.endswith('.lean') else rel
    if src == 'lean4' and r.startswith('src/'):
        r = r[4:]
    if src in ('lean4',) and r.startswith('src/lake/'):
        r = r
    return r.replace('/', '.')


# ------------------------------------------------------------------------------------------------ loading context
class Ctx:
    def __init__(self):
        self.db = conn_new(OUT + '.new')
        self.src = {s['name']: s for s in json.load(open(os.path.join(HERE, 'sources.json')))['sources']}
        self.man = {}
        with open(os.path.join(REPO, 'data', 'lean-db', 'manifest.tsv'), encoding='utf-8') as f:
            for r in csv.DictReader(f, delimiter='\t'):
                self.man.setdefault(r['name'], []).append(r)
        self.hdr = {}
        self.pool = Pool(A.procs)
        self.file_ids = {}

    def add_source(self, name):
        s = self.src[name]
        m = self.man.get(name, [])
        c = self.db
        c.execute('INSERT INTO sources(name,kind,use,license,url,record,revision,fetched_at,note,manifest_files,manifest_bytes) VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                  (name, s['kind'], s['use'], s.get('license'), s.get('url') or (('https://huggingface.co/datasets/' + s['repo']) if 'repo' in s else None),
                   s.get('record'), m[0]['revision'] if m else None, max((x['retrieved'] for x in m), default=None), s.get('note'),
                   len(m), sum(int(x['bytes']) for x in m)))

    def file_id(self, source, path, nbytes, sha, row_kind, status='ingested', note=None):
        k = (source, path)
        if k in self.file_ids:
            return self.file_ids[k]
        cur = self.db.execute('INSERT INTO files(source,path,bytes,sha256,row_kind,status,note) VALUES(?,?,?,?,?,?,?)',
                              (source, path, nbytes, sha, row_kind, status, note))
        self.file_ids[k] = cur.lastrowid
        return cur.lastrowid

    def add_rows(self, file_id, n):
        self.db.execute('UPDATE files SET rows=rows+? WHERE id=?', (n, file_id))

    def header_id(self, text):
        if not text:
            return None
        t = text.strip()
        h = sha(t)
        i = self.hdr.get(h)
        if i is None:
            cur = self.db.execute('INSERT INTO headers(sha256,text) VALUES(?,?)', (h, t))
            i = self.hdr[h] = cur.lastrowid
        return i


def register_raw_file(cx, source, path, kind, status='ingested', note=None, root=None):
    p = os.path.join(ROOT, path)
    return cx.file_id(source, path, os.path.getsize(p), sha256_file(p), kind, status, note)


# ------------------------------------------------------------------------------------------------ library repos
def load_library(cx, name):
    log('library', name)
    base = os.path.join(ROOT, name)
    rels = []
    skipped_dirs = ('.git/', '.lake/', 'build/', '.github/')
    for dp, dn, fn in os.walk(base):
        dn[:] = [d for d in dn if d not in ('.git', '.lake', 'build', 'node_modules')]
        for f in fn:
            if f.endswith('.lean'):
                rels.append(os.path.relpath(os.path.join(dp, f), base).replace(os.sep, '/'))
    rels.sort()
    if LIM:
        rels = rels[:LIM]
    jobs = [(base, name, r, area_of(r), module_of(name, r)) for r in rels]
    n = 0
    nb = 0
    seen_mod = set()
    c = cx.db
    for (rel, h, nbytes, rows, imps, err) in cx.pool.imap(parse_lean_file, jobs, chunksize=16):
        fid = cx.file_id(name, name + '/' + rel, nbytes, h, 'decls', 'ingested' if not err else 'skipped', err)
        mod = module_of(name, rel)
        nb += nbytes
        if mod not in seen_mod:
            seen_mod.add(mod)
            c.execute('INSERT OR IGNORE INTO module_imports VALUES(?,?,?,?)', (name, mod, name + '/' + rel, json.dumps(imps)))
        c.executemany('INSERT INTO decls(source,file_id,module,file,line,end_line,kind,name,full_name,namespace,area,signature,body,docstring,attrs,modifiers,norm_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                      [(name, fid) + r[:1] + (name + '/' + r[1],) + r[2:] for r in rows])
        cx.add_rows(fid, len(rows))
        n += len(rows)
    # non-.lean files: record as skipped context (hash only for non-huge files)
    other = 0
    for dp, dn, fn in os.walk(base):
        dn[:] = [d for d in dn if d not in ('.git',)]
        for f in fn:
            if not f.endswith('.lean'):
                other += 1
    c.execute('UPDATE sources SET files_ingested=?, bytes_ingested=?, n_decls=? WHERE name=?', (len(rels), nb, n, name))
    c.execute("INSERT OR REPLACE INTO meta VALUES(?,?)", ('other_files:' + name, str(other)))
    sha_rev = subprocess.run(['git', '-C', base, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    c.execute('UPDATE sources SET revision=? WHERE name=?', (sha_rev, name))
    c.commit()
    log('  ', name, len(rels), 'files', n, 'decls', other, 'other files not parsed')


def load_leandojo_tool(cx, name):
    base = os.path.join(ROOT, name)
    n = nb = 0
    for dp, dn, fn in os.walk(base):
        dn[:] = [d for d in dn if d != '.git']
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, ROOT).replace(os.sep, '/')
            cx.file_id(name, rel, os.path.getsize(p), sha256_file(p), 'none', 'skipped', 'tool source/docs of the LeanDojo tracer; contains no proof data')
            n += 1; nb += os.path.getsize(p)
    cx.db.execute("UPDATE sources SET files_ingested=0, bytes_ingested=0, note=coalesce(note,'')||? WHERE name=?",
                  ('; build: %d files hashed, none ingested (tool code, no data)' % n, name))
    cx.db.commit()


# ------------------------------------------------------------------------------------------------ problems helpers
def ins_problem(cx, row):
    cx.db.execute('INSERT INTO problems(source,use,file_id,orig_id,split,header_id,formal_statement,formal_proof,proof_form,informal_statement,'
                  'informal_proof,lean_version,mathlib_version,verified,verified_note,extra,norm_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)', row)


def nz(x):
    return None if x is None or (isinstance(x, str) and x.strip() in ('', 'None')) else x


def jd(o):
    return json.dumps(o, ensure_ascii=False, default=str) if o else None


def batched(it, n):
    b = []
    for x in it:
        b.append(x)
        if len(b) >= n:
            yield b
            b = []
    if b:
        yield b


def pq_rows(path, cols=None, bs=4000):
    import pyarrow.parquet as pq
    pf = pq.ParquetFile(path)
    got = 0
    for b in pf.iter_batches(batch_size=bs, columns=cols):
        for r in b.to_pylist():
            yield r
            got += 1
            if LIM and got >= LIM:
                return


def jsonl_rows(path):
    n = 0
    with open(path, encoding='utf-8') as f:
        for l in f:
            if l.strip():
                yield json.loads(l)
                n += 1
                if LIM and n >= LIM:
                    return


def run_extract(cx, items):
    """items: iterable of (key, text). returns iterator of (key, result) via pool, keeping memory bounded."""
    return cx.pool.imap(_w_extract, ((k, t, 0) for k, t in items), chunksize=64)


STAT = {}  # counts of rows read per source/file for reconciliation: STAT[(source, path)] = n


def finish_source(cx, name, files, ntable, nrows):
    nb = sum(os.path.getsize(os.path.join(ROOT, f)) for f in files)
    cx.db.execute(f'UPDATE sources SET files_ingested=?, bytes_ingested=?, {ntable}=? WHERE name=?', (len(files), nb, nrows, name))
    cx.db.commit()


def prob_from_extract(cx, src, use, fid, orig_id, split, ex, informal_s=None, informal_p=None, lean_v=None, ml_v=None,
                      verified=None, vnote=None, extra=None, header_text=None, proof_form=None, given_proof=None):
    """Build a problems row from an extract_file result (for datasets that ship whole files)."""
    extra = dict(extra or {})
    hdr = header_text if header_text is not None else ex.get('preamble')
    if ex.get('pre_code'):
        extra['pre_code'] = ex['pre_code']
    if not ex.get('ok'):
        extra['parse_fallback'] = True
    if ex.get('err'):
        extra['parse_error'] = ex['err']
    proof = given_proof if given_proof is not None else (ex.get('full_decl') if ex.get('proof') else None)
    pf = proof_form or ('full_decl' if proof is not None else None)
    stmt = ex.get('statement') or ''
    return (src, use, fid, orig_id, split, cx.header_id(hdr), stmt, proof, pf, nz(informal_s), nz(informal_p), lean_v, ml_v,
            verified, vnote, jd(extra), ex.get('nh') or LP.norm_hash(stmt))


# ------------------------------------------------------------------------------------------------ data/eval sources
def load_deepseek(cx):
    n = 0
    src = 'DeepSeek-Prover-V1'
    p = src + '/dataset.jsonl'
    fid = register_raw_file(cx, src, p, 'problems')
    for r in jsonl_rows(os.path.join(ROOT, p)):
        st = r['formal_statement']
        ex_extra = {'goal': r.get('goal'), 'name': r['name']}
        ins_problem(cx, (src, 'data', fid, r['name'], r['split'], cx.header_id(r.get('header')), st, nz(r.get('formal_proof')), 'tactics',
                         None, None, '4.9.0 (per dataset paper; not stated in card)', None, None,
                         'dataset proofs are the filtered, Lean-verified proofs per paper; not re-verified here' if nz(r.get('formal_proof')) else None,
                         jd(ex_extra), LP.norm_hash(st)))
        n += 1
    cx.add_rows(fid, n)
    finish_source(cx, src, [p], 'n_problems', n)
    STAT[(src, p)] = n


def load_goedel_lw(cx):
    src = 'Goedel-Lean-workbook-proofs'
    p = src + '/data/train-00000-of-00001.parquet'
    fid = register_raw_file(cx, src, p, 'problems')
    n = 0
    rows = list(pq_rows(os.path.join(ROOT, p)))
    for key, ex in run_extract(cx, ((i, r['full_proof']) for i, r in enumerate(rows))):
        ins_problem(cx, prob_from_extract(cx, src, 'data', fid, rows[key]['problem_id'], None, ex, lean_v='4.9.0 (Goedel-Prover-SFT env; not stated in card)',
                                          verified=None, vnote='dataset = solutions found by Goedel-Prover-SFT; the card does not state a verifier flag',
                                          extra={'source_set': 'Lean-workbook'}))
        n += 1
    cx.add_rows(fid, n)
    finish_source(cx, src, [p], 'n_problems', n)
    STAT[(src, p)] = n


def load_goedel_pset(cx):
    src = 'Goedel-Pset-v1'
    files = sorted(glob.glob(os.path.join(ROOT, src, 'data', '*.parquet')))
    rels = [os.path.relpath(f, ROOT).replace(os.sep, '/') for f in files]
    total = 0
    for rel in rels:
        fid = register_raw_file(cx, src, rel, 'problems')
        n = 0
        for chunk in batched(pq_rows(os.path.join(ROOT, rel)), 20000):
            for key, ex in run_extract(cx, ((i, r['formal_statement']) for i, r in enumerate(chunk))):
                r = chunk[key]
                ins_problem(cx, prob_from_extract(cx, src, 'data', fid, r['problem_id'], 'train', ex, informal_s=r['informal_statement'],
                                                  lean_v='4.9.0 (Goedel-Prover-V1 env; not stated in card)', verified=None,
                                                  vnote='statement-only (proof is sorry); card: statements passed CC test (compiles) and FC test', proof_form=None))
                n += 1
            cx.db.commit()
            log('   pset', rel[-26:], n)
        cx.add_rows(fid, n)
        STAT[(src, rel)] = n
        total += n
    finish_source(cx, src, rels, 'n_problems', total)


def load_herald(cx):
    src = 'Herald_proofs'
    p = src + '/data/train-00000-of-00001.parquet'
    fid = register_raw_file(cx, src, p, 'problems')
    n = 0
    for r in pq_rows(os.path.join(ROOT, p)):
        st = r['formal_theorem']
        ins_problem(cx, (src, 'data', fid, str(r['id']), None, cx.header_id(r.get('header')), st, nz(r.get('formal_proof')), 'full_decl',
                         r.get('informal_theorem'), r.get('informal_proof'), '4.11.0', 'Mathlib for Lean v4.11.0', None,
                         'Herald proofs are LLM-translated; the card does not state a verifier flag', jd({'name': r.get('name'), 'commented_proof': r.get('commented_proof')}), LP.norm_hash(st)))
        n += 1
    cx.add_rows(fid, n)
    finish_source(cx, src, [p], 'n_problems', n)
    STAT[(src, p)] = n


def load_lean_workbook(cx):
    src = 'Lean-Workbook'
    n = 0
    p = src + '/lean_workbook.json'
    fid = register_raw_file(cx, src, p, 'problems')
    d = json.load(open(os.path.join(ROOT, p), encoding='utf-8'))
    for k, r in enumerate(d if not LIM else d[:LIM]):
        st = r['formal_statement']
        m = re.match(r'\s*(?:theorem|lemma)\s+(\S+)', st)
        pr = r.get('proof')
        steps = None
        if isinstance(pr, str) and pr.strip() not in ('', '[]'):
            try:
                steps = ast.literal_eval(pr)
            except Exception:
                steps = None
        ins_problem(cx, (src, 'data', fid, m.group(1) if m else 'row%d' % k, r.get('split'), None, st,
                         json.dumps(steps, ensure_ascii=False) if steps else None, 'tactic_list_json' if steps else None,
                         r.get('natural_language_statement'), None, '4.8.0-rc1', 'Mathlib v4.8.0-rc1', None,
                         'proof field = list of tactic strings found by the authors; not a single script; no verifier flag', jd({'answer': r.get('answer'), 'tags': r.get('tags')}), LP.norm_hash(st)))
        n += 1
    cx.add_rows(fid, n)
    STAT[(src, p)] = n
    p2 = src + '/wkbk_1009.parquet'
    fid2 = register_raw_file(cx, src, p2, 'problems')
    n2 = 0
    for r in pq_rows(os.path.join(ROOT, p2)):
        st = r['formal_statement']
        ok = r.get('status') == 'proved'
        ins_problem(cx, (src, 'data', fid2, r['id'], 'wkbk_1009', None, st, nz(r.get('tactic')), 'tactics', r.get('natural_language_statement'), None,
                         '4.8.0-rc1', 'Mathlib v4.8.0-rc1', 1 if ok else 0, 'status=%s (dataset flag)' % r.get('status'),
                         jd({'answer': r.get('answer'), 'state_before': r.get('state_before'), 'state_after': r.get('state_after'), 'status': r.get('status')}), LP.norm_hash(st)))
        n2 += 1
    cx.add_rows(fid2, n2)
    STAT[(src, p2)] = n2
    finish_source(cx, src, [p, p2], 'n_problems', n + n2)


def load_numina(cx):
    src = 'NuminaMath-LEAN'
    p = src + '/data/train-00000-of-00001.parquet'
    fid = register_raw_file(cx, src, p, 'problems')
    n = 0
    rows = list(pq_rows(os.path.join(ROOT, p)))
    for key, ex in run_extract(cx, ((i, r['formal_statement']) for i, r in enumerate(rows))):
        r = rows[key]
        gt = {key: r['formal_ground_truth']}
        gtt = r.get('ground_truth_type')
        g = nz(gt[key]) if gtt in ('complete', 'with_sorry') else None
        proof = None; form = None
        if g:
            gex = extract_file(g)
            proof = gex['full_decl'] if gex.get('proof') else None
            form = 'full_decl'
        elif nz(r.get('formal_proof')):
            proof = r['formal_proof']; form = 'unknown_form'
        extra = {k: r.get(k) for k in ('question_type', 'answer', 'author', 'ground_truth_type', 'source', 'problem_type', 'exam', 'rl_data')}
        if g and nz(r.get('formal_proof')):
            extra['formal_proof_rl'] = r['formal_proof']
        vnote = {'complete': 'ground_truth_type=complete: card says formalisations are compiled and verified against mathlib v4.15.0',
                 'with_sorry': 'ground truth contains sorry in some have steps', 'statement': 'statement only'}.get(gtt, gtt)
        ins = prob_from_extract(cx, src, 'data', fid, r['uuid'], None, ex, informal_s=r['problem'], lean_v='4.15.0', ml_v='Mathlib v4.15.0',
                                verified=(1 if gtt == 'complete' else None), vnote=vnote, extra=extra, proof_form=form, given_proof=proof)
        ins_problem(cx, ins)
        n += 1
    cx.add_rows(fid, n)
    finish_source(cx, src, [p], 'n_problems', n)
    STAT[(src, p)] = n


def load_formalmath(cx):
    src = 'FormalMATH-All'
    p = src + '/FormalMATH_all.json'
    fid = register_raw_file(cx, src, p, 'problems')
    d = json.load(open(os.path.join(ROOT, p), encoding='utf-8'))
    d = d if not LIM else d[:LIM]
    n = 0
    for key, ex in run_extract(cx, ((str(i), r['autoformalization']) for i, r in enumerate(d))):
        r = d[int(key)]
        ins_problem(cx, prob_from_extract(cx, src, 'eval', fid, r.get('theorem_names') or key, r.get('source'), ex, informal_s=r.get('refined_statement'),
                                          informal_p=r.get('solution'), lean_v='4.9.0 (not stated in the data)',
                                          verified=(1 if str(r.get('compiler_feedback_bool')) == 'True' else 0),
                                          vnote='compiler_feedback_bool (statement compiled with sorry)',
                                          extra={'domain': r.get('domain'), 'compiler_feedback': r.get('compiler_feedback')}))
        n += 1
    cx.add_rows(fid, n)
    finish_source(cx, src, [p], 'n_problems', n)
    STAT[(src, p)] = n


def load_mobench(cx):
    src = 'MathOlympiadBench'
    p = src + '/MOBench.jsonl'
    fid = register_raw_file(cx, src, p, 'problems')
    rows = list(jsonl_rows(os.path.join(ROOT, p)))
    n = 0
    for key, ex in run_extract(cx, ((str(i), r['lean4_code']) for i, r in enumerate(rows))):
        r = rows[int(key)]
        stm = r['formal_statement']
        hdr = (r.get('header') or '').strip()
        solved = str(r.get('solved')) == 'True'
        proof = ex.get('full_decl') if ex.get('proof') else None
        extra = {'tags': r.get('tags'), 'category': r.get('category'), 'problem_id': r.get('problem_id'), 'solved': r.get('solved')}
        if ex.get('pre_code'):
            extra['pre_code'] = ex['pre_code']
        ins_problem(cx, (src, 'eval', fid, r['name'], r.get('split'), cx.header_id(hdr), stm, proof, 'full_decl' if proof else None,
                         r.get('informal_prefix'), None, None, None, 1 if solved else 0, "'solved' flag of the dataset", jd(extra), LP.norm_hash(stm)))
        n += 1
    cx.add_rows(fid, n)
    finish_source(cx, src, [p], 'n_problems', n)
    STAT[(src, p)] = n


def load_proofnet_lean4(cx):
    src = 'ProofNet-lean4'
    p = src + '/proofnet_lean4.jsonl'
    fid = register_raw_file(cx, src, p, 'problems')
    n = 0
    for r in jsonl_rows(os.path.join(ROOT, p)):
        st = r['formal_statement']
        ins_problem(cx, (src, 'eval', fid, r['name'], None, cx.header_id(r.get('header')), st, nz(r.get('formal_proof')), 'unknown_form',
                         r.get('informal_stmt'), r.get('informal_proof'), '4.20.0', None, None, None, jd({'source_file': r.get('source')}), LP.norm_hash(st)))
        n += 1
    cx.add_rows(fid, n)
    files = [p]
    for dp, dn, fn in os.walk(os.path.join(ROOT, src)):
        dn[:] = [d for d in dn if d != '.git']
        for f in fn:
            fp = os.path.join(dp, f)
            rel = os.path.relpath(fp, ROOT).replace(os.sep, '/')
            if rel != p:
                cx.file_id(src, rel, os.path.getsize(fp), sha256_file(fp), 'none', 'skipped',
                           'secondary copy of the same problems (lean/tex/pdf/py) or tooling; jsonl is the ingested view')
    finish_source(cx, src, files, 'n_problems', n)
    STAT[(src, p)] = n


def load_minif2f_hf(cx):
    src = 'minif2f-lean4-hf'
    files = []
    tot = 0
    for sp in ('test', 'valid'):
        p = '%s/%s.jsonl' % (src, sp)
        fid = register_raw_file(cx, src, p, 'problems')
        files.append(p)
        n = 0
        for r in jsonl_rows(os.path.join(ROOT, p)):
            st = r['formal_statement']
            ins_problem(cx, (src, 'eval', fid, r['id'], r.get('split') or sp, cx.header_id(r.get('header')), st, None, None, r.get('informal_stmt'),
                             r.get('informal_proof'), None, None, None, None, None, LP.norm_hash(st)))
            n += 1
        cx.add_rows(fid, n); STAT[(src, p)] = n; tot += n
    finish_source(cx, src, files, 'n_problems', tot)


def load_proofnet_hf(cx):
    src = 'proofnet-hf'
    files = []
    tot = 0
    for sp in ('test', 'valid'):
        p = '%s/%s.jsonl' % (src, sp)
        fid = register_raw_file(cx, src, p, 'problems')
        files.append(p)
        n = 0
        for r in jsonl_rows(os.path.join(ROOT, p)):
            st = r['formal_statement']
            ins_problem(cx, (src, 'eval', fid, r['id'], sp, cx.header_id(r.get('src_header')), st, None, None, r.get('nl_statement'), r.get('nl_proof'),
                             'Lean 3 (mathlib3)', None, None, None, None, LP.norm_hash(st)))
            n += 1
        cx.add_rows(fid, n); STAT[(src, p)] = n; tot += n
    for f in ('common.lean', 'proofnet.py', 'test_loader.py', 'proofnet.png', 'README.md', '.gitattributes'):
        rel = src + '/' + f
        if os.path.exists(os.path.join(ROOT, rel)):
            cx.file_id(src, rel, os.path.getsize(os.path.join(ROOT, rel)), sha256_file(os.path.join(ROOT, rel)), 'none', 'skipped',
                       'support/doc file (Lean 3 common.lean prelude, loader, card)')
    finish_source(cx, src, files, 'n_problems', tot)


def _file_problem_rows(cx, src, files_rel, use, split_of, lean_v):
    """One problem per .lean file (miniF2F-lean4, PutnamBench)."""
    n = 0
    items = []
    for rel in files_rel:
        t = open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace').read()
        items.append((rel, t))
    docs = {}
    for rel, t in items:
        _m, ds = LP.mask(t)
        docs[rel] = ds[-1][2] if ds else None
    for rel, ex in run_extract(cx, items):
        fid = cx.file_ids.get((src, rel)) or register_raw_file(cx, src, rel, 'problems')
        if not ex.get('ok'):
            cx.db.execute("UPDATE files SET status='skipped', note=?, row_kind='none' WHERE id=?", ('no declaration found (aggregator/support file)', fid))
            continue
        stem = os.path.basename(rel)[:-5]
        ins_problem(cx, prob_from_extract(cx, src, use, fid, stem, split_of(rel), ex, informal_s=docs[rel], lean_v=lean_v))
        cx.add_rows(fid, 1)
        n += 1
    return n


def load_minif2f_git(cx):
    src = 'miniF2F-lean4'
    base = os.path.join(ROOT, src)
    rels = sorted(os.path.relpath(os.path.join(dp, f), ROOT).replace(os.sep, '/') for dp, dn, fn in os.walk(base)
                  if '.git' not in dp for f in fn if f.endswith('.lean'))
    n = _file_problem_rows(cx, src, rels, 'eval', lambda r: 'test' if '/Test/' in r else ('valid' if '/Valid/' in r else None), 'v4.24.0 (repo toolchain)')
    finish_source(cx, src, rels, 'n_problems', n)


def load_putnam(cx):
    src = 'PutnamBench'
    base = os.path.join(ROOT, src, 'lean4', 'src')
    rels = sorted(os.path.relpath(os.path.join(base, f), ROOT).replace(os.sep, '/') for f in os.listdir(base) if f.endswith('.lean'))
    n = _file_problem_rows(cx, src, rels, 'eval', lambda r: 'putnam', 'v4.33.1 (repo toolchain)')
    # other proof assistants / docs: hash only
    for dp, dn, fn in os.walk(os.path.join(ROOT, src)):
        dn[:] = [d for d in dn if d != '.git']
        for f in fn:
            fp = os.path.join(dp, f)
            rel = os.path.relpath(fp, ROOT).replace(os.sep, '/')
            if (src, rel) not in cx.file_ids:
                cx.file_id(src, rel, os.path.getsize(fp), sha256_file(fp), 'none', 'skipped',
                           'non-Lean (Isabelle/Coq/informal/docs) or Lean tooling, not a problem file')
    finish_source(cx, src, rels, 'n_problems', n)


# ------------------------------------------------------------------------------------------------ LeanDojo benchmarks
def ld_line(pos):
    try:
        return int(pos[0])
    except Exception:
        return None


def parse_pos(s):
    if isinstance(s, (list, tuple)):
        return ld_line(s)
    m = re.match(r'\s*\[\s*(\d+)', s or '')
    return int(m.group(1)) if m else None


def load_leandojo_benchmark(cx, name, tar_rel):
    import ijson
    log('leandojo benchmark', name)
    tar_path = os.path.join(ROOT, tar_rel)
    tar_fid = register_raw_file(cx, name, tar_rel, 'none', note='tarball; members listed below')
    c = cx.db
    thm_ids = {}      # (file,full_name,start) -> theorem id
    thm_tacs = {}     # theorem id -> list of tactic strings (to detect inconsistent splits)
    counts = {}
    members = {}
    with tarfile.open(tar_path, 'r|gz') as tf:
        for m in tf:
            if not m.isfile():
                continue
            rel = m.name.split('/', 1)[1]
            f = tf.extractfile(m)
            full = tar_rel + '!' + rel
            if rel == 'corpus.jsonl':
                fid = cx.file_id(name, full, m.size, None, 'decls')
                n = nd = 0
                h = hashlib.sha256()
                for lb in iter(f.readline, b''):
                    if not lb.strip():
                        continue
                    h.update(lb)
                    r = json.loads(lb.decode('utf-8'))
                    path = r['path']
                    mod = module_of('leandojo', re.sub(r'^\.lake/packages/[^/]+/(?:src/)?(?:lean/)?', '', path)) if path.startswith('.lake/packages') else module_of('leandojo', path)
                    if path.startswith('.lake/packages/lean4/src/lean/'):
                        mod = module_of('leandojo', path[len('.lake/packages/lean4/src/lean/'):])
                    c.execute('INSERT OR IGNORE INTO module_imports VALUES(?,?,?,?)', (name, mod, path, json.dumps(r.get('imports', []))))
                    rows = []
                    for pr in r.get('premises', []):
                        code = pr.get('code') or ''
                        ds = LP.parse_decls(code)
                        d0 = ds[0] if ds else None
                        sig = d0.signature if d0 else code
                        body = d0.body if d0 else ''
                        kind = d0.kind if d0 else (pr.get('kind') or 'unknown')
                        rows.append((name, fid, mod, path, ld_line(pr.get('start')), ld_line(pr.get('end')), kind, pr['full_name'].split('.')[-1] if pr.get('full_name') else None,
                                     pr.get('full_name'), None, 'src', sig, body, d0.docstring if d0 else None, d0.attrs if d0 else None, d0.modifiers if d0 else None,
                                     json.dumps({'corpus_kind': pr.get('kind'), 'code_sha256': sha(code)}), LP.norm_hash((('@[' + d0.attrs + '] ') if d0 and d0.attrs else '') + sig)))
                    c.executemany('INSERT INTO decls(source,file_id,module,file,line,end_line,kind,name,full_name,namespace,area,signature,body,docstring,attrs,modifiers,extra,norm_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)', rows)
                    nd += len(rows)
                    n += 1
                    if LIM and n >= LIM:
                        break
                c.execute('UPDATE files SET sha256=?, rows=? WHERE id=?', ('stream:' + h.hexdigest(), nd, fid))
                counts['corpus_files'] = n
                counts['corpus_premises'] = nd
                c.commit()
                log('  corpus', n, 'files', nd, 'premises')
            elif rel.endswith('/train.json') or rel.endswith('/val.json') or rel.endswith('/test.json'):
                kind, sp = rel.split('/')
                sp = sp[:-5]
                fid = cx.file_id(name, full, m.size, None, 'theorems')
                nth = ntr = nnew = nmatch = ndiff = 0
                col = 'split_random' if kind == 'random' else 'split_novel'
                for r in ijson.items(f, 'item', use_float=True):
                    key = (r['file_path'], r['full_name'], tuple(r.get('start') or []))
                    tacs = r.get('traced_tactics') or []
                    tid = thm_ids.get(key)
                    tseq = [t['tactic'] for t in tacs]
                    if tid is not None and thm_tacs.get(tid) == tseq:
                        c.execute(f'UPDATE trace_theorems SET {col}=? WHERE id=?', (sp, tid))
                        nmatch += 1
                        nth += 1
                        continue
                    if tid is not None:
                        ndiff += 1  # same theorem key but different tactics: store as a separate theorem (flagged below)
                    stmt = r.get('theorem_statement')
                    cur = c.execute('INSERT INTO trace_theorems(source,file_id,repo,commit_sha,file,full_name,start_line,end_line,statement,n_tactics,' + col + ',norm_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                                    (name, fid, r.get('url'), r.get('commit'), r['file_path'], r['full_name'], parse_pos(r.get('start')), parse_pos(r.get('end')),
                                     stmt, len(tacs), sp, LP.norm_hash(stmt) if stmt else None))
                    tid = cur.lastrowid
                    if (tid is not None) and key not in thm_ids:
                        thm_ids[key] = tid
                        thm_tacs[tid] = tseq
                    nnew += 1
                    nth += 1
                    for i, t in enumerate(tacs):
                        ann = t.get('annotated_tactic') or [None, []]
                        prem = ann[1] if len(ann) > 1 and ann[1] else []
                        cur2 = c.execute('INSERT INTO traces(theorem_id,source,idx,tactic,state_before,state_after,n_premises) VALUES(?,?,?,?,?,?,?)',
                                         (tid, name, i, t['tactic'], t.get('state_before'), t.get('state_after'), len(prem)))
                        if prem:
                            c.executemany('INSERT INTO trace_premises(trace_id,full_name,def_path,def_line) VALUES(?,?,?,?)',
                                          [(cur2.lastrowid, q.get('full_name'), q.get('def_path'), ld_line(q.get('def_pos'))) for q in prem])
                        ntr += 1
                    if LIM and nth >= LIM:
                        break
                c.execute('UPDATE files SET rows=? WHERE id=?', (nth, fid))
                counts[rel] = {'theorems': nth, 'new': nnew, 'matched_other_split': nmatch, 'inconsistent': ndiff, 'tactics': ntr}
                c.commit()
                log('  ', rel, counts[rel])
            elif rel == 'traced_files.jsonl':
                cx.file_id(name, full, m.size, None, 'none', 'skipped', 'list of traced file paths; informational')
                counts['traced_files'] = sum(1 for _ in f)
            else:
                data = f.read()
                cx.file_id(name, full, m.size, hashlib.sha256(data).hexdigest(), 'none', 'skipped', 'metadata / licence text')
                if rel == 'metadata.json':
                    counts['metadata'] = json.loads(data)
    c.execute("UPDATE sources SET files_ingested=?, bytes_ingested=?, note=coalesce(note,'')||? WHERE name=?",
              (4, os.path.getsize(tar_path), '; build counts: ' + json.dumps({k: v for k, v in counts.items() if k != 'metadata'}), name))
    nd = c.execute('SELECT count(*) FROM decls WHERE source=?', (name,)).fetchone()[0]
    nt = c.execute('SELECT count(*) FROM traces WHERE source=?', (name,)).fetchone()[0]
    nth = c.execute('SELECT count(*) FROM trace_theorems WHERE source=?', (name,)).fetchone()[0]
    c.execute('UPDATE sources SET n_decls=?, n_traces=?, n_theorems=? WHERE name=?', (nd, nt, nth, name))
    c.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', ('leandojo_counts:' + name, json.dumps(counts)))
    c.commit()


def load_lean_github(cx):
    src = 'Lean-Github'
    p = src + '/lean-github.parquet'
    fid = register_raw_file(cx, src, p, 'traces')
    c = cx.db
    thm = {}
    n = 0
    for r in pq_rows(os.path.join(ROOT, p)):
        key = (r['url'], r['commit'], r['file_path'], r['full_name'], r['start'])
        tid = thm.get(key)
        if tid is None:
            cur = c.execute('INSERT INTO trace_theorems(source,file_id,repo,commit_sha,file,full_name,start_line,end_line,n_tactics) VALUES(?,?,?,?,?,?,?,?,0)',
                            (src, fid, r['url'], r['commit'], r['file_path'], r['full_name'], parse_pos(r['start']), parse_pos(r['end'])))
            tid = thm[key] = cur.lastrowid
        c.execute('UPDATE trace_theorems SET n_tactics=n_tactics+1 WHERE id=?', (tid,))
        c.execute('INSERT INTO traces(theorem_id,source,idx,tactic,state_before,state_after,n_premises) VALUES(?,?,?,?,?,?,0)',
                  (tid, src, 0, r['tactic'], r['state_before'], r['state_after']))
        n += 1
    c.execute('UPDATE traces SET idx=(SELECT count(*) FROM traces t2 WHERE t2.theorem_id=traces.theorem_id AND t2.id<traces.id) WHERE source=?', (src,))
    cx.add_rows(fid, n)
    STAT[(src, p)] = n
    finish_source(cx, src, [p], 'n_traces', n)
    c.execute('UPDATE sources SET n_theorems=? WHERE name=?', (len(thm), src))
    c.commit()


# ------------------------------------------------------------------------------------------------ post-processing
def post_resolve_premises(cx):
    c = cx.db
    log('index for premise resolution')
    c.executescript(INDEXES)
    c.execute("CREATE TEMP TABLE m_ff AS SELECT source, full_name, file, min(id) AS id FROM decls WHERE full_name IS NOT NULL AND full_name!='' GROUP BY 1,2,3")
    c.execute('CREATE UNIQUE INDEX temp.m_ff_i ON m_ff(source, full_name, file)')
    c.execute("CREATE TEMP TABLE m_f AS SELECT source, full_name, min(id) AS id FROM decls WHERE full_name IS NOT NULL AND full_name!='' GROUP BY 1,2")
    c.execute('CREATE UNIQUE INDEX temp.m_f_i ON m_f(source, full_name)')
    c.execute("CREATE TEMP TABLE m_lib AS SELECT d.full_name, min(d.id) AS id FROM decls d JOIN sources s ON s.name=d.source WHERE s.use='library' AND d.full_name!='' GROUP BY 1")
    c.execute('CREATE UNIQUE INDEX temp.m_lib_i ON m_lib(full_name)')
    for bench in ('leandojo-benchmark-4-v10', 'leandojo-benchmark-4-2026'):
        c.execute("""UPDATE trace_premises SET decl_id=(SELECT m.id FROM m_ff m WHERE m.source=? AND m.full_name=trace_premises.full_name AND m.file=trace_premises.def_path)
                     WHERE trace_id IN (SELECT id FROM traces WHERE source=?)""", (bench, bench))
        c.execute("""UPDATE trace_premises SET decl_id=(SELECT m.id FROM m_f m WHERE m.source=? AND m.full_name=trace_premises.full_name)
                     WHERE decl_id IS NULL AND trace_id IN (SELECT id FROM traces WHERE source=?)""", (bench, bench))
        c.commit()
        log('  resolved', bench)
    c.execute('UPDATE trace_premises SET lib_decl_id=(SELECT m.id FROM m_lib m WHERE m.full_name=trace_premises.full_name)')
    c.commit()


def minhash_batch(texts):
    """worker: per text -> None (too short) or (minhash64 bytes, shingle-hash bytes)."""
    import numpy as np
    rng = np.random.RandomState(12345)
    a = rng.randint(1, 2 ** 31 - 1, size=64).astype(np.uint64)
    b = rng.randint(0, 2 ** 31 - 1, size=64).astype(np.uint64)
    out = []
    for t in texts:
        tk = LP.tokens(t)
        if len(tk) < 12:
            out.append(None)
            continue
        sh = {zlib.crc32(' '.join(tk[i:i + 3]).encode()) for i in range(len(tk) - 2)}
        h = np.fromiter(sh, dtype=np.uint64)
        sig = ((h[None, :] * a[:, None] + b[:, None]) % np.uint64(4294967291)).min(axis=1)
        out.append((sig.astype(np.uint32).tobytes(), np.sort(h).astype(np.uint32).tobytes()))
    return out


def post_overlap(cx):
    """Contamination: non-eval rows whose normalised statement equals (exact norm_hash) or nearly equals
    (token 3-shingle Jaccard >= 0.9, found with a 64-perm minhash / 16x4 LSH and verified exactly) an eval statement."""
    import numpy as np
    c = cx.db
    c.execute('CREATE TABLE overlap_pairs(tbl TEXT, row_id INTEGER, eval_problem_id INTEGER, how TEXT)')
    log('contamination: exact')
    r = {}
    for tbl, extra in (('decls', ''), ('problems', " AND t.use!='eval'"), ('trace_theorems', '')):
        c.execute(f"""INSERT INTO overlap_pairs SELECT '{tbl}', t.id, (SELECT min(p.id) FROM problems p WHERE p.use='eval' AND p.norm_hash=t.norm_hash), 'exact'
                      FROM {tbl} t WHERE t.norm_hash IN (SELECT norm_hash FROM problems WHERE use='eval' AND norm_hash IS NOT NULL){extra}""")
        c.execute(f"UPDATE {tbl} SET eval_overlap=1 WHERE id IN (SELECT row_id FROM overlap_pairs WHERE tbl='{tbl}')")
        r[tbl] = c.execute("SELECT count(*) FROM overlap_pairs WHERE tbl=? AND how='exact'", (tbl,)).fetchone()[0]
    c.commit()
    log('  exact overlaps', r)
    log('contamination: near-duplicate')
    ev = c.execute("SELECT id, formal_statement FROM problems WHERE use='eval'").fetchall()
    shingles = {}
    bands = {}
    for (i, t), res in zip(ev, minhash_batch([LP.norm_statement(t) for _, t in ev])):
        if res:
            s = np.frombuffer(res[0], dtype=np.uint32)
            shingles[i] = set(np.frombuffer(res[1], dtype=np.uint32).tolist())
            for bnd in range(16):
                bands.setdefault((bnd, s[bnd * 4:bnd * 4 + 4].tobytes()), []).append(i)
    log('  eval statements with signature:', len(shingles))
    near = {}

    def scan(tbl, sql):
        cur = c.execute(sql)
        hits = 0
        while True:
            rows = cur.fetchmany(20000)
            if not rows:
                break
            res = cx.pool.map(minhash_batch, [[LP.norm_statement(x[1]) for x in rows[k:k + 500]] for k in range(0, len(rows), 500)])
            flat = [x for ch in res for x in ch]
            pairs = []
            for (rid, _t), rr in zip(rows, flat):
                if not rr:
                    continue
                s = np.frombuffer(rr[0], dtype=np.uint32)
                cand = set()
                for bnd in range(16):
                    cand.update(bands.get((bnd, s[bnd * 4:bnd * 4 + 4].tobytes()), ()))
                if not cand:
                    continue
                mine = set(np.frombuffer(rr[1], dtype=np.uint32).tolist())
                for e in sorted(cand):
                    if len(mine & shingles[e]) / max(1, len(mine | shingles[e])) >= 0.9:
                        pairs.append((tbl, rid, e, 'near'))
                        break
            if pairs:
                c.executemany('INSERT INTO overlap_pairs VALUES(?,?,?,?)', pairs)
                c.executemany(f'UPDATE {tbl} SET eval_overlap=1 WHERE id=?', [(p[1],) for p in pairs])
                hits += len(pairs)
            c.commit()
        return hits
    for tbl, sql in (
        ('decls', "SELECT id, CASE WHEN attrs IS NULL OR attrs='' THEN signature ELSE '@['||attrs||'] '||signature END FROM decls WHERE eval_overlap=0"),
        ('problems', "SELECT id, formal_statement FROM problems WHERE use!='eval' AND eval_overlap=0"),
        ('trace_theorems', "SELECT id, statement FROM trace_theorems WHERE eval_overlap=0 AND statement IS NOT NULL"),
    ):
        near[tbl] = scan(tbl, sql)
        log('  near', tbl, near[tbl])
    c.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', ('overlap_counts', json.dumps({'exact': r, 'near': near})))
    c.commit()


def post_fts(cx):
    c = cx.db
    log('FTS5 indexes (non-eval, non-overlap rows only)')
    c.executescript('''
    CREATE VIRTUAL TABLE search_idx USING fts5(name, statement, docstring, ident_split,
        decl_id UNINDEXED, source UNINDEXED, kind UNINDEXED, area UNINDEXED, module UNINDEXED, tokenize="unicode61 remove_diacritics 0");
    CREATE VIRTUAL TABLE examples_idx USING fts5(name, statement, informal, ident_split,
        problem_id UNINDEXED, source UNINDEXED, has_proof UNINDEXED, tokenize="unicode61 remove_diacritics 0");
    ''')
    # decls: library sources + same-benchmark corpus of the verified (2024 v10) benchmark; 2026 corpus excluded (unverified provenance)
    c.execute("""UPDATE decls SET searchable=0 WHERE eval_overlap=1 OR source IN (SELECT name FROM sources WHERE provenance='unverified')""")
    cur = c.execute('''SELECT d.id, d.full_name, d.name, d.signature, d.docstring, d.source, d.kind, d.area, d.module FROM decls d WHERE d.searchable=1''')
    n = 0
    while True:
        rows = cur.fetchmany(50000)
        if not rows:
            break
        c.executemany('INSERT INTO search_idx(name,statement,docstring,ident_split,decl_id,source,kind,area,module) VALUES(?,?,?,?,?,?,?,?,?)',
                      [((fn or nm or ''), (sig or '')[:3000], (doc or '')[:2000], ident_split((fn or nm or '') + ' ' + (sig or '')[:1500]), i, s, k, a, mo)
                       for (i, fn, nm, sig, doc, s, k, a, mo) in rows])
        n += len(rows)
    c.execute("CREATE VIRTUAL TABLE search_vocab USING fts5vocab(search_idx, 'row')")
    log('  search_idx rows', n)
    # examples: data problems, non-overlap, first occurrence per norm_hash (prefer rows with a proof)
    cur = c.execute('''SELECT id, orig_id, formal_statement, informal_statement, source, formal_proof IS NOT NULL FROM problems
                       WHERE use='data' AND eval_overlap=0 AND id IN (SELECT min(id) FROM problems WHERE use='data' AND eval_overlap=0 GROUP BY norm_hash)''')
    m = 0
    while True:
        rows = cur.fetchmany(50000)
        if not rows:
            break
        c.executemany('INSERT INTO examples_idx(name,statement,informal,ident_split,problem_id,source,has_proof) VALUES(?,?,?,?,?,?,?)',
                      [(oid or '', (st or '')[:3000], (inf or '')[:1500], ident_split((st or '')[:1500]), i, s, hp) for (i, oid, st, inf, s, hp) in rows])
        m += len(rows)
    log('  examples_idx rows', m)
    c.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', ('search_idx_rows', str(n)))
    c.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', ('examples_idx_rows', str(m)))
    c.commit()


def git_commit():
    try:
        h = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', REPO, 'status', '--porcelain', '--', 'tools/lean_db'], capture_output=True, text=True).stdout.strip()
        return h + ('+dirty' if dirty else '')
    except Exception:
        return 'unknown'


def main():
    t0 = time.time()
    cx = Ctx()
    only = set(A.only.split(',')) if A.only else None
    for s in cx.src:
        cx.add_source(s)
    # provenance notes
    note26 = ('Zenodo 18815372 (CC-BY-4.0), uploaded 2026-02-28 by Ryan Hsiang (not the original LeanDojo author Kaiyu Yang, whose official '
              'record is 12740403); description: "traced on v4.29.0-rc2 of Mathlib4"; metadata.json: leandojo_version 4.20.0, traced with a LeanDojo-v2 fork on a '
              'TACC cluster, from mathlib4 commit 1bc7728a (verified to exist: Kim Morrison, 2026-02-24). The tarball content was not re-traced or '
              'independently reproduced here. Treated as UNVERIFIED PROVENANCE: stored, but its corpus decls are excluded from search_idx.')
    cx.db.execute("""UPDATE sources SET provenance='unverified', note=? WHERE name='leandojo-benchmark-4-2026'""", (note26,))
    cx.db.execute("""UPDATE sources SET provenance='confirmed', note=coalesce(note,'')||'; official Zenodo record 12740403 by Kaiyu Yang (Caltech), from mathlib4 commit 29dcec07 (2024-07-01), LeanDojo 2.0.0, Lean v4.10.0-rc1 era' WHERE name='leandojo-benchmark-4-v10'""")
    cx.db.commit()
    steps = [
        ('lean4', lambda: load_library(cx, 'lean4')), ('batteries', lambda: load_library(cx, 'batteries')),
        ('mathlib4', lambda: load_library(cx, 'mathlib4')), ('lean-smt', lambda: load_library(cx, 'lean-smt')),
        ('LeanDojo', lambda: load_leandojo_tool(cx, 'LeanDojo')),
        ('DeepSeek-Prover-V1', lambda: load_deepseek(cx)), ('Goedel-Lean-workbook-proofs', lambda: load_goedel_lw(cx)),
        ('Herald_proofs', lambda: load_herald(cx)), ('Lean-Workbook', lambda: load_lean_workbook(cx)),
        ('NuminaMath-LEAN', lambda: load_numina(cx)), ('Lean-Github', lambda: load_lean_github(cx)),
        ('FormalMATH-All', lambda: load_formalmath(cx)), ('MathOlympiadBench', lambda: load_mobench(cx)),
        ('ProofNet-lean4', lambda: load_proofnet_lean4(cx)), ('minif2f-lean4-hf', lambda: load_minif2f_hf(cx)),
        ('proofnet-hf', lambda: load_proofnet_hf(cx)), ('miniF2F-lean4', lambda: load_minif2f_git(cx)),
        ('PutnamBench', lambda: load_putnam(cx)),
        ('leandojo-benchmark-4-v10', lambda: load_leandojo_benchmark(cx, 'leandojo-benchmark-4-v10', 'leandojo-benchmark-4-v10/leandojo_benchmark_4.tar.gz')),
        ('leandojo-benchmark-4-2026', lambda: load_leandojo_benchmark(cx, 'leandojo-benchmark-4-2026', 'leandojo-benchmark-4-2026/mathlib4.tar.gz')),
        ('Goedel-Pset-v1', lambda: load_goedel_pset(cx)),
    ]
    for nm, fn in steps:
        if only and nm not in only:
            continue
        log('==', nm)
        fn()
        cx.db.commit()
    for name, ms in cx.man.items():
        for m in ms:
            rel = m['path']
            if rel.endswith('/') or (name, rel) in cx.file_ids or not os.path.exists(os.path.join(ROOT, rel)):
                continue
            if only and name not in only:
                continue
            cx.file_id(name, rel, int(m['bytes']), m['sha256'], 'none', 'skipped', 'card/licence/metadata/binary; no rows (sha256 from manifest)')
    cx.db.execute("UPDATE files SET rows=0 WHERE rows IS NULL")
    post_resolve_premises(cx)
    post_overlap(cx)
    post_fts(cx)
    cx.db.executescript(VIEWS)
    cx.db.execute('UPDATE sources SET files_ingested=coalesce(files_ingested,0), bytes_ingested=coalesce(bytes_ingested,0)')
    for k, v in (('schema_version', SCHEMA_VERSION), ('build_date', datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')),
                 ('script_git_commit', git_commit()), ('build_seconds', str(int(time.time() - t0))), ('limit_rows', str(LIM)),
                 ('python', sys.version.split()[0]), ('sqlite', sqlite3.sqlite_version)):
        cx.db.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', (k, v))
    cx.db.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', ('read_counts', json.dumps({'%s::%s' % k: v for k, v in STAT.items()})))
    cx.db.commit()
    log('ANALYZE / VACUUM-free finish')
    cx.db.execute('ANALYZE')
    cx.db.commit()
    cx.db.close()
    cx.pool.close()
    os.replace(OUT + '.new', OUT)
    log('done', OUT, os.path.getsize(OUT) // (1 << 20), 'MiB', int(time.time() - t0), 's')


if __name__ == '__main__':
    main()
