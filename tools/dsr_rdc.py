#!/usr/bin/env python3
"""Name the constant buffers of a replayed DSR RenderDoc capture (tools/dsr-rdc.sh runs this after the replay).

Chain: captured SPIR-V (its OpString "fs.<hash>") -> DXVK's dump of that shader (<stage>.<hash>.dxbc, DXVK_SHADER_DUMP_PATH)
-> the DXBC's RDEF chunk (cbuffer + variable names, offsets, types) -> the DSR shaderbnd entry with identical DXBC
bytes (shader/*_DX11.shaderbnd.dcx, e.g. FRPG_Fil_ToneMap.fpo). Values come from the replayed buffer bytes.

    dsr_rdc.py FRAME.json SHADER_DUMP_DIR OUT.json [--all]
    dsr_rdc.py glsl SHADER_DUMP_DIR OUT_DIR [NAME_SUBSTRING]   decompile DXVK's SPIR-V of DSR shaders to GLSL
        (SPIRV-Cross, env SPIRV_CROSS; files named after the shaderbnd entry, e.g. FRPG_Fil_HDR_PBL_ColAdj.fpo.glsl,
        with the RDEF cbuffer layout as a header comment: DXVK's GLSL indexes cb0[N] = register cN)
OUT: {"summary": {stage:var: [{value, count, first_eid, shaders}]},  every named (non DL_FREG) variable used by a
      shader, its distinct values over the frame (gFC_FogCol, gVC_FogParam, gFC_HemAmbCol_u, gFC_Ls*, ...),
      "tonemap": the FRPG_Fil_HDR_PBL_ColAdj registers decoded (exposure, Hable, ToneCorrect matrix),
      "shaders": {name: {...}}, "passes": [{eid, name, vs, ps, cbuffers: {cbname: {var: value}}}]}.
Without --all only the first event per unique (shader pair, constant values) is kept.
"""
import glob, hashlib, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
from gamepath import game_dir


def chunks(dxbc):
    n = struct.unpack_from('<I', dxbc, 28)[0]
    for i in range(n):
        off = struct.unpack_from('<I', dxbc, 32 + 4 * i)[0]
        yield dxbc[off:off + 4], dxbc[off + 8:off + 8 + struct.unpack_from('<I', dxbc, off + 4)[0]]


def cstr(b, o):
    return b[o:b.index(b'\0', o)].decode('latin-1')


def rdef(dxbc):
    """[{name, register, size, vars: [{name, offset, size, cls, type, rows, cols, elements}]}] (cbuffers only)."""
    c = dict(chunks(dxbc)).get(b'RDEF')
    if not c:
        return []
    ncb, cboff, nbind, bindoff, minor, major = struct.unpack_from('<IIIIBB', c, 0)
    vsz = 40 if major >= 5 else 24
    regs = {}
    for i in range(nbind):
        name, typ, _, _, _, point, count, _ = struct.unpack_from('<8I', c, bindoff + 32 * i)
        if typ == 0:  # D3D_SIT_CBUFFER
            regs[cstr(c, name)] = point
    out = []
    for i in range(ncb):
        name, nvar, varoff, size, _, _ = struct.unpack_from('<6I', c, cboff + 24 * i)
        cb = {'name': cstr(c, name), 'register': regs.get(cstr(c, name)), 'size': size, 'vars': []}
        for k in range(nvar):
            vname, start, vsize, flags, typeoff = struct.unpack_from('<5I', c, varoff + vsz * k)
            cls, typ, rows, cols, elems, members, _ = struct.unpack_from('<6HI', c, typeoff)
            cb['vars'].append({'name': cstr(c, vname), 'offset': start, 'size': vsize, 'used': bool(flags & 2),
                               'cls': cls, 'type': typ, 'rows': rows, 'cols': cols, 'elements': elems})
        out.append(cb)
    return out


def value(data, v):
    """Floats / ints of one variable (16-byte register packing for matrices and arrays)."""
    if v['offset'] + 4 > len(data):
        return None
    fmt = 'i' if v['type'] in (2, 19) else 'I' if v['type'] in (1, 20) else 'f'  # int / uint|bool / float
    n_el = max(1, v['elements'])
    rows = v['rows'] if v['cls'] in (2, 3) else 1  # matrix rows (column_major=3 stores columns, kept as stored)
    cols = v['cols']
    vals = []
    for e in range(n_el):
        for r in range(rows):
            base = v['offset'] + (e * rows + r) * 16
            if n_el == 1 and rows == 1:
                base = v['offset']
            if base + 4 * cols > len(data):
                break
            vals.extend(struct.unpack_from('<%d%s' % (cols, fmt), data, base))
    vals = [round(x, 6) if isinstance(x, float) else x for x in vals]
    return vals[0] if len(vals) == 1 else vals


def shaderbnd_names():
    names = {}
    for f in sorted(glob.glob(os.path.join(game_dir(), 'shader', '*_DX11.shaderbnd.dcx'))):
        for _, name, data in souls.bnd3(souls.read(f)):
            if data[:4] == b'DXBC':
                names.setdefault(hashlib.sha1(data).hexdigest(), []).append(
                    os.path.basename(f).split('.')[0] + '/' + name.replace('\\', '/').split('/')[-1])
    return names


def glsl(dump, outdir, pat=''):
    import subprocess
    sc = os.environ.get('SPIRV_CROSS', os.path.join(os.environ.get('DSR_REF', os.path.join(os.environ.get('DISTROBOX_HOST_HOME', os.path.expanduser('~')), 'dsr-ref')), 'tools/dl/spirv-cross/build/spirv-cross'))
    bnd = shaderbnd_names()
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for dx in sorted(glob.glob(os.path.join(dump, '*.dxbc'))):
        data = open(dx, 'rb').read()
        names = bnd.get(hashlib.sha1(data).hexdigest())
        if not names or pat not in ' '.join(names):
            continue
        r = subprocess.run([sc, dx[:-5] + '.spv', '--version', '450', '--vulkan-semantics'], capture_output=True, text=True)
        head = '\n'.join('// %s b%s %s: %s' % (cb['name'], cb['register'], cb['size'], ', '.join(
            '%s@c%d%s' % (v['name'], v['offset'] // 16, '' if v['used'] else '(unused)') for v in cb['vars']))
            for cb in rdef(data))
        for name in names:
            open(os.path.join(outdir, name.split('/')[-1] + '.glsl'), 'w').write(
                '// %s (DXVK %s)\n%s\n%s' % (name, os.path.basename(dx), head, r.stdout or r.stderr))
            n += 1
    print('%d shaders decompiled to %s' % (n, outdir))


def main():
    if sys.argv[1] == 'glsl':
        return glsl(*sys.argv[2:])
    frame, dump, out = sys.argv[1:4]
    keep_all = '--all' in sys.argv
    fr = json.load(open(frame))
    bnd = shaderbnd_names()
    spv2 = {}  # captured spv sha1 -> (dxvk name, dxbc bytes)
    for s in glob.glob(os.path.join(dump, '*.spv')):
        dx = s[:-4] + '.dxbc'
        if os.path.exists(dx):
            spv2[hashlib.sha1(open(s, 'rb').read()).hexdigest()] = (os.path.basename(s)[:-4], open(dx, 'rb').read())
    shaders, passes, seen, unmatched = {}, [], set(), set()

    def shader(h, dxvk=None):
        dx = os.path.join(dump, (dxvk or '') + '.dxbc')
        if dxvk and os.path.exists(dx):
            dname, dxbc = dxvk, open(dx, 'rb').read()
        elif h in spv2:
            dname, dxbc = spv2[h]
        else:
            unmatched.add(dxvk or h)
            return None
        names = bnd.get(hashlib.sha1(dxbc).hexdigest(), [dname])
        key = names[0]
        if key not in shaders:
            shaders[key] = {'dxvk': dname, 'aliases': names[1:], 'cbuffers': rdef(dxbc)}
        return key

    for ev in fr['events']:
        row = {'eid': ev['eid'], 'name': ev['name'], 'cbuffers': {}}
        sig = []
        for st, s in ev['stages'].items():
            key = shader(s['spv_sha1'], s.get('dxvk'))
            row[st] = key or s['spv_sha1']
            if not key:
                continue
            cbs = shaders[key]['cbuffers']
            for cb in s['cbs']:
                # DXVK binds D3D11 cbuffer register N; RenderDoc names DXVK's blocks after the register
                reg = cb.get('binding')
                desc = next((d for d in cbs if d['register'] is not None and d['name'] and
                             cb['name'] in (d['name'], 'cb%d' % d['register'])), None)
                if desc is None:
                    desc = next((d for d in cbs if d['size'] == cb['size']), None)
                if desc is None:
                    continue
                data = bytes.fromhex(fr['blobs'][cb['blob']])
                row['cbuffers'][st + ':' + desc['name']] = {v['name']: value(data, v) for v in desc['vars']}
                sig.append((st, desc['name'], cb['blob']))
        k = (row.get('vs'), row.get('ps'), row.get('cs'), tuple(sig))
        if keep_all or k not in seen:
            seen.add(k)
            passes.append(row)
    summary = {}
    for p in passes:
        for st in ('vs', 'ps', 'cs'):
            sh = shaders.get(p.get(st))
            if not sh:
                continue
            used = {v['name'] for cb in sh['cbuffers'] for v in cb['vars'] if v['used']}
            for cbn, vals in p['cbuffers'].items():
                if not cbn.startswith(st + ':'):
                    continue
                for var, val in vals.items():
                    if var in used and not var.startswith(('DL_FREG', 'VR_')) and 'Mtx' not in var:
                        lst = summary.setdefault(st + ':' + var, [])
                        hit = next((e for e in lst if e['value'] == val), None)
                        if hit is None:
                            lst.append({'value': val, 'count': 1, 'first_eid': p['eid'], 'shaders': [p[st]]})
                        else:
                            hit['count'] += 1
                            if p[st] not in hit['shaders'] and len(hit['shaders']) < 4:
                                hit['shaders'].append(p[st])
    tonemap = None
    for p in passes:
        if str(p.get('ps')).endswith('FRPG_Fil_HDR_PBL_ColAdj.fpo'):
            g = p['cbuffers'].get('ps:$Globals', {})
            r = lambda n: g.get('DL_FREG_%03d' % n)
            c54, c55, c56, c57, m, c68, c71 = r(54), r(55), r(56), r(57), r(62), r(68), r(71)
            tonemap = {'eid': p['eid'], 'source': 'FRPG_Fil_HDR_PBL_ColAdj.fpo $Globals (register cN = DL_FREG_0NN)',
                       'grayKey_c54x': c54[0], 'white_c54y': c54[1], 'hable': {'A': c55[0], 'B': c55[1], 'C': c55[2],
                       'D': c55[3], 'E': c54[2], 'F': c54[3]}, 'adaptedLum_min_max_c56zw': c56[2:4],
                       'bloom_weight_c56x_c71y': [c56[0], c71[1]], 'tex3_weight_c56y': c56[1], 'mode_c57x': c57[0],
                       'toneCorrect_matrix_c62_c64': [m[0:4], m[4:8], m[8:12]], 'overlay_weight_c68x': c68[0],
                       'c71': c71}
            break
    json.dump({'capture_events': len(fr['events']), 'unmatched_spirv': sorted(unmatched), 'tonemap': tonemap,
               'summary': summary, 'shaders': shaders, 'passes': passes}, open(out, 'w'), indent=1)
    print('%d events, %d unique passes, %d named shaders, %d unmatched SPIR-V' %
          (len(fr['events']), len(passes), len(shaders), len(unmatched)))


if __name__ == '__main__':
    main()
