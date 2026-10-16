#!/usr/bin/env python3
"""Cloth test cases cut out of game recordings (.pxcr), shrunk, and replayed as a suite.

A case is a small .pxcr: the meshes it needs, the whole solver state before its first step (kRecState), the calls of
its steps, and the solver's own position hash after every step. Cut and checked with the i686 Windows replay
(tools/cloth_replay_w32.sh, the build that reproduces the game); the native i686 build is used to speed up the search
when it gives the same hashes. Next to each case, <case>.json holds what it is about and its limits.

Published cases live in tests/cloth_cases/ as .pxcr.zst (zstd; the zstd command is needed). A case may only go there
when it holds no level triangle mesh and no machine path (the tool refuses otherwise; such a case stays in
captures/cases/, which is not published). What a published case holds from the game: the cloth's rest mesh, the
primitive shapes it touches, and the calls of its steps. The full recordings are never published.

  cloth_case.py cut REC A B OUT.pxcr [--note TEXT] [--no-min] [-- replay options for the full recording]
      steps A..B of REC as a case (the recording is replayed with --dual --keep unless replay options are given),
      then shrunk (see min) and its limits written from its own metrics.
  cloth_case.py min CASE.pxcr
      drops what the case does not need while every step's hash stays the same: shapes the cloth never meets (made
      unreachable, so shape ids stay), meshes no shape uses, mesh triangles that change nothing.
  cloth_case.py publish CASE.pxcr tests/cloth_cases/NAME.pxcr.zst
      copies a case (and its .json) to the published folder, if it may go there (no level mesh, no machine path).
  cloth_case.py run [CASE | DIR ...]   (default: tests/cloth_cases and captures/cases)
      replays each case: "exact" (all hashes match), "changed" (the solver's result differs: expected after a
      behaviour change, then check the limits and rebase), and any limit broken (a spark, a fall-through, NaN).
  cloth_case.py rebase CASE
      takes the current solver's hashes as the case's own (after an intended behaviour change). Only the .json changes:
      the case file holds no hashes, so published binaries change only on a recut. The case still starts
      from the state the old solver reached; to start from what the current one reaches, recut.
  cloth_case.py recut [CASE.pxcr | DIR ...]
      cuts each case again from its source recording (source, steps and replay options are in its .json; the note and
      limits are kept): after a solver change that the case should see from the recording on, or a state layout change
      that the case no longer loads with. Needs the recordings: keep them (privately) as the cases' source.

Limits (in the .json): vmax (m/s, fastest particle), stretch (longest triangle edge / rest), fall (particles through a
floor), nonfinite (positions), ms (slowest step; informative, not checked unless "check_ms": true).
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W32 = os.path.join(ROOT, 'tools', 'cloth_replay_w32.sh')
NATIVE = os.path.join(ROOT, 'build', 'replay-native', 'cloth_replay')
PUBLIC_DIR = os.path.join(ROOT, 'tests', 'cloth_cases')
PRIVATE_DIR = os.path.join(ROOT, 'captures', 'cases')
TMP_DIR = os.path.join(ROOT, 'build', 'cases-tmp')

OP_STATE, OP_MESH, OP_ADDSHAPE, OP_NOTE, OP_STEP, OP_CHECK = 25, 2, 3, 20, 18, 19
FAR_SPHERE = struct.pack('<I3ff3fI', 1, 1e7, 1e7, 1e7, 0.0, 0.0, 0.0, 0.0, 0xffffffff)


# ---- the .pxcr ops -------------------------------------------------------------------------------------------------

def _vec(b, o, elem):
    n = struct.unpack_from('<I', b, o)[0]
    return o + 4 + n * elem


def op_end(b, o):
    """End offset of the op whose payload starts at o (after its op byte)."""
    op = b[o - 1]
    if op == 1:  # create: params, rest, tri, x0, has edges [edges], stored rest
        o = _vec(b, o, 1); o = _vec(b, o, 12); o = _vec(b, o, 4); o = _vec(b, o, 12)
        has = b[o]; o += 1
        if has:
            o = _vec(b, o, 28)
        return o + 1
    if op == OP_MESH:
        return _vec(b, o + 4, 12)
    if op == 6:  # motion
        o += 4 + 12 + 36
        if b[o]: o += 12
        o += 1
        if b[o]: o += 36
        return o + 2
    if op in (17, 24, OP_STATE):
        return _vec(b, o, 1)
    if op == OP_NOTE:
        return o + 4 + struct.unpack_from('<I', b, o)[0]
    fixed = {3: 36, 4: 16, 5: 8, 7: 4, 8: 24, 9: 16, 10: 4, 11: 0, 12: 16, 13: 24, 14: 41, 15: 4, 16: 4, 18: 16,
             19: 4, 21: 16, 22: 28, 23: 40}
    if op not in fixed:
        raise ValueError('unknown op %d at %d' % (op, o - 1))
    return o + fixed[op]


def read_ops(path):
    b = path if isinstance(path, bytes) else open(path, 'rb').read()
    if b[:4] != b'PXCR':
        raise ValueError('not a PXCR file')
    ops, o = [], 8
    while o < len(b):
        e = op_end(b, o + 1)
        ops.append([b[o], bytes(b[o + 1:e])])
        o = e
    return b[:8], ops


def write_ops(path, head, ops):
    with open(path, 'wb') as f:
        f.write(head)
        for op, payload in ops:
            f.write(bytes([op]))
            f.write(payload)


class State:
    """The tagged fields of a kRecState payload (Cloth::saveState), editable."""

    def __init__(self, payload):
        n = struct.unpack_from('<I', payload, 0)[0]
        b = payload[4:4 + n]
        self.head = b[:8]
        count = struct.unpack_from('<I', b, 8)[0]
        self.fields, o = [], 12
        for _ in range(count):
            ln = b[o]; name = b[o + 1:o + 1 + ln].decode(); o += 1 + ln
            sz = struct.unpack_from('<I', b, o)[0]; o += 4
            self.fields.append([name, bytes(b[o:o + sz])]); o += sz

    def get(self, name):
        for f in self.fields:
            if f[0] == name:
                return f[1]
        return None

    def put(self, name, data):
        for f in self.fields:
            if f[0] == name:
                f[1] = data
                return
        raise KeyError(name)

    def payload(self):
        out = bytearray(self.head + struct.pack('<I', len(self.fields)))
        for name, data in self.fields:
            nb = name.encode()
            out += bytes([len(nb)]) + nb + struct.pack('<I', len(data)) + data
        return struct.pack('<I', len(out)) + bytes(out)

    def shapes(self):  # list of 36-byte records
        d = self.get('shapes_')
        return [d[4 + 36 * i:40 + 36 * i] for i in range((len(d) - 4) // 36)]

    def set_shapes(self, recs):
        self.put('shapes_', struct.pack('<I', 36) + b''.join(recs))

    def positions(self):
        d = self.get('x_')
        return [struct.unpack_from('<3f', d, 4 + 12 * i) for i in range((len(d) - 4) // 12)]


class Case:
    def __init__(self, path):
        self.head, self.ops = read_ops(path)
        k = [i for i, (op, _) in enumerate(self.ops) if op == OP_STATE]
        if len(k) != 1:
            raise ValueError('%s: not a case (one state op expected, found %d)' % (path, len(k)))
        self.si = k[0]
        self.state = State(self.ops[self.si][1])

    def meshes(self):  # id -> op index
        return {struct.unpack_from('<I', p, 0)[0]: i for i, (op, p) in enumerate(self.ops) if op == OP_MESH}

    def mesh_tris(self, mid):
        p = self.ops[self.meshes()[mid]][1]
        n = struct.unpack_from('<I', p, 4)[0]
        return [p[8 + 36 * t:8 + 36 * (t + 1)] for t in range(n // 3)]

    def set_mesh_tris(self, mid, tris):
        i = self.meshes()[mid]
        self.ops[i][1] = struct.pack('<II', mid, 3 * len(tris)) + b''.join(tris)

    def used_meshes(self):
        ids = set()
        for r in self.state.shapes():
            ids.add(struct.unpack_from('<I', r, 32)[0])
        for op, p in self.ops:
            if op == OP_ADDSHAPE:
                ids.add(struct.unpack_from('<I', p, 32)[0])
        ids.discard(0xffffffff)
        return ids

    def write(self, path):
        self.ops[self.si][1] = self.state.payload()
        write_ops(path, self.head, self.ops)

    def steps(self):
        return sum(1 for op, _ in self.ops if op == OP_STEP)


# ---- case files: .pxcr, or .pxcr.zst (published) -------------------------------------------------------------------

def is_zst(p):
    return p.endswith('.zst')


def sidecar(p):
    base = p[:-4] if is_zst(p) else p
    return os.path.splitext(base)[0] + '.json'


def load_meta(p):
    return json.load(open(sidecar(p))) if os.path.exists(sidecar(p)) else {}


def save_meta(p, meta):
    with open(sidecar(p), 'w') as f:
        json.dump(meta, f, indent=1)
        f.write('\n')


def materialize(p):
    """A temporary plain .pxcr of case p, with the expected hashes of its .json as check ops after every step (the case
    file holds none, so a rebase changes only the .json). Returns (path, True); the caller deletes it."""
    data = open(p, 'rb').read()
    if data.startswith(b'version https://git-lfs'):
        raise SystemExit('%s is a Git LFS pointer: fetch the cases with "git lfs install && git lfs pull"' % p)
    if is_zst(p):
        data = subprocess.run(['zstd', '-d', '-q', '-c'], input=data, stdout=subprocess.PIPE, check=True).stdout
    head, ops = read_ops(data)
    hashes = load_meta(p).get('hashes')
    if hashes and not any(op == OP_CHECK for op, _ in ops):
        out, k = [], 0
        for op, pl in ops:
            out.append([op, pl])
            if op == OP_STEP and k < len(hashes):
                out.append([OP_CHECK, struct.pack('<I', int(hashes[k], 16))])
                k += 1
        ops = out
    os.makedirs(TMP_DIR, exist_ok=True)
    fd, raw = tempfile.mkstemp(suffix='.pxcr', dir=TMP_DIR)
    os.close(fd)
    write_ops(raw, head, ops)
    return raw, True


def hashes_of(raw):
    """The check ops of a plain case (one per step), as hex strings."""
    return ['%08x' % struct.unpack('<I', pl)[0] for op, pl in read_ops(raw)[1] if op == OP_CHECK]


def is_public(p):
    return os.path.abspath(p).startswith(PUBLIC_DIR + os.sep)


def publish_problems(raw):
    """Why case raw (a path or the bytes of a plain .pxcr) may not be published (empty: it may)."""
    c = Case(raw)
    out = []
    if c.meshes():
        out.append('%d level triangle mesh(es)' % len(c.meshes()))
    mesh_shapes = sum(1 for r in c.state.shapes() if struct.unpack_from('<I', r, 32)[0] != 0xffffffff)
    mesh_shapes += sum(1 for op, pl in c.ops if op == OP_ADDSHAPE and struct.unpack_from('<I', pl, 32)[0] != 0xffffffff)
    if mesh_shapes:
        out.append('%d mesh shape(s)' % mesh_shapes)
    data = raw if isinstance(raw, bytes) else open(raw, 'rb').read()
    for pat in (b'/' + b'home/', b'/' + b'Users/', b':\\' + b'Users', b':/' + b'Users', b'Z:/'):  # split: no path in this file
        if pat in data:
            out.append('a machine path (%r)' % pat)
    return out


def store(raw, out):
    """Writes case raw to out (compressed if out is .zst) without its check ops, and returns their hashes (they go to
    the .json); refuses a published case that holds game meshes or paths."""
    if is_public(out):
        if not is_zst(out):
            raise SystemExit('%s: published cases are .pxcr.zst' % out)
        why = publish_problems(raw)
        if why:
            raise SystemExit('%s: not published, the case holds %s; keep it in captures/cases/' % (out, ', '.join(why)))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    head, ops = read_ops(raw)
    hashes = ['%08x' % struct.unpack('<I', pl)[0] for op, pl in ops if op == OP_CHECK]
    fd, bare = tempfile.mkstemp(suffix='.pxcr', dir=TMP_DIR)
    os.close(fd)
    write_ops(bare, head, [o for o in ops if o[0] != OP_CHECK])
    if is_zst(out):
        subprocess.run(['zstd', '-19', '-q', '-f', bare, '-o', out], check=True)
        os.unlink(bare)
    else:
        os.replace(bare, out)
    return hashes


# ---- replays -------------------------------------------------------------------------------------------------------

def build_native():
    src = [os.path.join(ROOT, 'solver', 'cloth', f) for f in sorted(os.listdir(os.path.join(ROOT, 'solver', 'cloth')))
           if f.startswith('cloth') and f.endswith('.cpp')]
    deps = src + [os.path.join(ROOT, 'solver', 'cloth', 'cloth.h'), os.path.join(ROOT, 'solver', 'cloth', 'cloth_rec.h')]
    if os.path.exists(NATIVE) and all(os.path.getmtime(d) <= os.path.getmtime(NATIVE) for d in deps):
        return True
    os.makedirs(os.path.dirname(NATIVE), exist_ok=True)
    r = subprocess.run(['clang++', '-m32', '-O2', '-std=c++17', '-pthread', '-I' + os.path.join(ROOT, 'solver')] + src +
                       ['-o', NATIVE], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write('native build failed (search runs on the Windows build only):\n' + r.stderr[-2000:])
        return False
    return True


def winpath(p):
    return 'Z:' + os.path.abspath(p)


def replay(path, args=(), native=False):
    cmd = [NATIVE, path] if native else ['bash', W32, winpath(path)]
    r = subprocess.run(cmd + list(args), capture_output=True, text=True)
    return r.stdout + r.stderr, r.returncode


def check(path, native=False):
    """(exact, metrics): exact = every hash matched; metrics from --metrics --fall."""
    out, rc = replay(path, ['--quiet', '--metrics', '--fall'], native)
    exact, m = False, {'vmax': 0.0, 'stretch': 0.0, 'fall': 0, 'nonfinite': 0, 'ms': 0.0, 'steps': 0}
    for line in out.splitlines():
        if line.startswith('M '):
            f = line.split()
            m['steps'] += 1
            m['vmax'] = max(m['vmax'], float(f[4]))
            m['stretch'] = max(m['stretch'], float(f[6]))
            m['nonfinite'] = max(m['nonfinite'], int(f[7]))
            m['fall'] = max(m['fall'], int(f[8]))
            m['ms'] = max(m['ms'], float(f[9]))
        elif 'hash checks' in line:
            seg = line.split('hash checks')[1]
            checks = int(seg.split(',')[0])
            bad = int(seg.split('mismatches')[1].split()[0])
            exact = rc == 0 and checks > 0 and bad == 0
    return exact, m


# ---- shrinking -----------------------------------------------------------------------------------------------------

def shrink(path, log=print):
    case = Case(path)
    use_native = build_native() and check(path, native=True)[0]
    log('  search on the %s build' % ('native' if use_native else 'Windows'))
    tmp = tempfile.NamedTemporaryFile(suffix='.pxcr', delete=False, dir=os.path.dirname(os.path.abspath(path)))
    tmp.close()
    evals = [0]

    def ok():
        case.write(tmp.name)
        evals[0] += 1
        return check(tmp.name, native=use_native)[0]

    def ddmin(items, drop, restore):
        """Drops as many of items as possible: drop(subset) applies, restore(subset) undoes; ok() decides."""
        kept = []
        stack = [list(items)]
        while stack:
            part = stack.pop()
            if not part:
                continue
            drop(part)
            if ok():
                continue
            restore(part)
            if len(part) == 1:
                kept.append(part[0])
                continue
            h = len(part) // 2
            stack.append(part[h:])
            stack.append(part[:h])
        return kept

    # shapes the cloth never meets: unreachable spheres (ids stay), in the state and among the window's addShape calls
    # (a shim rebuild adds every shape again)
    shapes = case.state.shapes()
    orig_state = list(shapes)
    orig_ops = {i: p for i, (op, p) in enumerate(case.ops) if op == OP_ADDSHAPE}
    units = [('s', i) for i, r in enumerate(shapes) if r != FAR_SPHERE]
    units += [('o', i) for i, p in orig_ops.items() if p != FAR_SPHERE]

    def set_far(part, far):
        cur = case.state.shapes()
        for kind, i in part:
            if kind == 's':
                cur[i] = FAR_SPHERE if far else orig_state[i]
            else:
                case.ops[i][1] = FAR_SPHERE if far else orig_ops[i]
        case.state.set_shapes(cur)

    kept = ddmin(units, lambda p: set_far(p, True), lambda p: set_far(p, False))
    log('  shapes: %d of %d kept (%d replays)' % (len(kept), len(units), evals[0]))

    # calls that change nothing now (motion, body mass and velocity of shapes made unreachable, detaches, notes after
    # the first): dropped
    droppable = [i for i, (op, _) in enumerate(case.ops) if op in (5, 6, 7, 23) or (op == OP_NOTE and i > 0)]
    gone = set()

    def rebuild_ops():
        case.ops = [o for k, o in enumerate(all_ops) if k not in gone]
        case.si = [k for k, (op, _) in enumerate(case.ops) if op == OP_STATE][0]

    all_ops = list(case.ops)
    e0 = evals[0]
    keep_ops = ddmin(droppable, lambda p: (gone.update(p), rebuild_ops()), lambda p: (gone.difference_update(p), rebuild_ops()))
    rebuild_ops()
    log('  shape calls: %d of %d kept (%d replays)' % (len(keep_ops), len(droppable), evals[0] - e0))

    # meshes no shape uses
    used = case.used_meshes()
    before = len(case.meshes())
    case.ops = [o for o in case.ops if not (o[0] == OP_MESH and struct.unpack_from('<I', o[1], 0)[0] not in used)]
    case.si = [i for i, (op, _) in enumerate(case.ops) if op == OP_STATE][0]
    if not ok():
        raise RuntimeError('dropping unused meshes changed the result')
    log('  meshes: %d of %d kept' % (len(case.meshes()), before))

    # triangles, nearest to the cloth last (far ones go in big halves first)
    xs = case.state.positions()
    lo = [min(p[k] for p in xs) for k in range(3)]
    hi = [max(p[k] for p in xs) for k in range(3)]

    def dist(tri):  # mesh frame; the meshes of moving shapes are not where the cloth is, which only slows the search
        v = struct.unpack('<9f', tri)
        c = [(v[k] + v[k + 3] + v[k + 6]) / 3 for k in range(3)]
        return sum(max(lo[k] - c[k], 0, c[k] - hi[k]) ** 2 for k in range(3))

    for mid in sorted(case.meshes()):
        tris = case.mesh_tris(mid)
        order = sorted(range(len(tris)), key=lambda t: -dist(tris[t]))
        alive = set(range(len(tris)))

        def apply(part, gone):
            alive.difference_update(part) if gone else alive.update(part)
            case.set_mesh_tris(mid, [tris[t] for t in sorted(alive)])

        e0 = evals[0]
        keep = ddmin(order, lambda p: apply(p, True), lambda p: apply(p, False))
        log('  mesh %d: %d of %d triangles kept (%d replays)' % (mid, len(keep), len(tris), evals[0] - e0))

    case.write(tmp.name)
    if use_native and not check(tmp.name)[0]:
        os.unlink(tmp.name)
        raise RuntimeError('the shrunk case is exact on the native build but not on the Windows build: kept the original')
    os.replace(tmp.name, path)
    return evals[0]


# ---- commands ------------------------------------------------------------------------------------------------------

def limits_from(m):
    return {'vmax': round(max(m['vmax'] * 1.25, m['vmax'] + 5.0), 1), 'stretch': round(max(m['stretch'] * 1.25, 1.5), 2),
            'fall': m['fall'], 'nonfinite': 0}


def cmd_cut(argv):
    rest = []
    if '--' in argv:
        k = argv.index('--')
        argv, rest = argv[:k], argv[k + 1:]
    note, do_min = '', True
    if '--note' in argv:
        k = argv.index('--note')
        note = argv[k + 1]
        del argv[k:k + 2]
    if '--no-min' in argv:
        argv.remove('--no-min')
        do_min = False
    rec, a, b, out = argv
    os.makedirs(TMP_DIR, exist_ok=True)
    raw = os.path.join(TMP_DIR, os.path.basename(out).split('.')[0] + '.cut.pxcr')
    args = rest or ['--dual', '--keep']
    txt, rc = replay(os.path.abspath(rec), ['--quiet'] + args + ['--cut', a, b, winpath(raw)])
    if rc or not os.path.exists(raw):
        sys.exit('cut failed:\n' + txt[-2000:])
    for line in txt.splitlines():
        if line.startswith('cut:'):
            print(line)
    exact, m = check(raw)
    if not exact:
        sys.exit('%s: the case does not replay exactly (state not complete?)' % out)
    size0 = os.path.getsize(raw)
    if do_min:
        shrink(raw)
    exact, m = check(raw)
    hashes = store(raw, out)
    os.unlink(raw)
    save_meta(out, {'source': os.path.relpath(os.path.abspath(rec), ROOT), 'steps': [int(a), int(b)],
                    'replay_args': args, 'note': note, 'metrics': m, 'limits': limits_from(m), 'hashes': hashes})
    print('%s: %d steps, %.0f KB stored (cut %.0f KB), exact %s, vmax %.1f stretch %.2f fall %d' % (
        out, m['steps'], os.path.getsize(out) / 1024, size0 / 1024, exact, m['vmax'], m['stretch'], m['fall']))


def cmd_min(argv):
    for p in argv:
        raw, _ = materialize(p)
        s0 = os.path.getsize(raw)
        n = shrink(raw)
        s1 = os.path.getsize(raw)
        meta = load_meta(p)
        meta['hashes'] = store(raw, p)
        save_meta(p, meta)
        os.unlink(raw)
        print('%s: %.0f -> %.0f KB (stored %.0f KB), %d replays' % (p, s0 / 1024, s1 / 1024, os.path.getsize(p) / 1024, n))


def cases_in(argv):
    out = []
    for p in argv or [d for d in (PUBLIC_DIR, PRIVATE_DIR) if os.path.isdir(d)]:
        if os.path.isdir(p):
            out += sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith(('.pxcr', '.pxcr.zst')))
        else:
            out.append(p)
    return out


def cmd_run(argv):
    bad = 0
    for p in cases_in(argv):
        meta = load_meta(p)
        lim = meta.get('limits', {})
        raw, _ = materialize(p)
        exact, m = check(raw)
        os.unlink(raw)
        broken = [k for k in ('vmax', 'stretch', 'fall', 'nonfinite') if k in lim and m[k] > lim[k]]
        if meta.get('check_ms') and 'ms' in lim and m['ms'] > lim['ms']:
            broken.append('ms')
        state = 'exact' if exact else 'changed'
        print('%-48s %-7s vmax %7.1f stretch %5.2f fall %3d nonfinite %d max %6.2f ms%s' % (
            os.path.relpath(p, ROOT), state, m['vmax'], m['stretch'], m['fall'], m['nonfinite'], m['ms'],
            ('  LIMIT: ' + ', '.join('%s %s > %s' % (k, m[k], lim[k]) for k in broken)) if broken else ''))
        bad += bool(broken)
    sys.exit(1 if bad else 0)


def cmd_rebase(argv):
    """New expected hashes from the current solver; the case file itself stays as it is."""
    for p in argv:
        raw, _ = materialize(p)
        new = os.path.join(TMP_DIR, 'rebase.pxcr')
        steps = Case(raw).steps()
        txt, rc = replay(raw, ['--quiet', '--cut', '0', str(steps - 1), winpath(new)])
        os.unlink(raw)
        if rc or not os.path.exists(new):
            sys.exit('rebase failed:\n' + txt[-2000:])
        meta = load_meta(p)
        meta['hashes'] = hashes_of(new)
        os.unlink(new)
        save_meta(p, meta)
        raw, _ = materialize(p)
        exact, m = check(raw)
        os.unlink(raw)
        meta['metrics'] = m
        save_meta(p, meta)
        print('%s: rebased (.json only), exact %s' % (p, exact))


def cmd_publish(argv):
    src, dst = argv
    raw, _ = materialize(src)
    meta = load_meta(src)
    meta['hashes'] = store(raw, dst)
    os.unlink(raw)
    save_meta(dst, meta)
    print('%s: %.0f KB' % (dst, os.path.getsize(dst) / 1024))


def cmd_recut(argv):
    for p in cases_in(argv):
        meta = json.load(open(sidecar(p)))
        src = os.path.join(ROOT, meta['source'])
        if not os.path.exists(src):
            print('%s: source %s missing, not recut' % (p, meta['source']))
            continue
        lim = meta.get('limits')
        cmd_cut([src, str(meta['steps'][0]), str(meta['steps'][1]), p, '--note', meta.get('note', ''), '--'] +
                meta.get('replay_args', ['--dual', '--keep']))
        if lim:  # limits chosen for the case stay
            m = load_meta(p)
            m['limits'] = lim
            save_meta(p, m)


def main():
    cmds = {'cut': cmd_cut, 'min': cmd_min, 'run': cmd_run, 'rebase': cmd_rebase, 'recut': cmd_recut,
            'publish': cmd_publish}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])


if __name__ == '__main__':
    main()
