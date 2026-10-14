#!/usr/bin/env python3
"""Decode a PhysXLoader_shim.log: NxFluidDesc dumps (layout: docs/re-notes-fluid-mesh.md 3.1) and call summary.
usage: analysis/shim_log.py captures/game-run/<ts>/Binaries_shim.log"""
import re, struct, sys

F = lambda w: struct.unpack('<f', struct.pack('<I', w))[0]
SIM = {1: 'SPH', 2: 'NO_INTERACTION', 4: 'MIXED'}

def decode(d):
    fl = lambda i: round(F(d[i]), 6)
    pw = d[0x35:0x45]
    return {
        'maxParticles': d[0x10], 'numReserve': d[0x11],
        'restPPM': fl(0x12), 'restDensity': fl(0x13), 'KRM': fl(0x14), 'motionLimitM': fl(0x15),
        'collDistM': fl(0x16), 'packetSizeM': d[0x17],
        'stiffness': fl(0x18), 'viscosity': fl(0x19), 'surfTension': fl(0x1a), 'damping': fl(0x1b), 'fadeIn': fl(0x1c),
        'extAccel': [fl(i) for i in range(0x1d, 0x20)], 'projPlane': [fl(i) for i in range(0x20, 0x24)],
        'restS/dynFrS/statFrS/attrS': [fl(i) for i in range(0x24, 0x28)],
        'restD/dynFrD/statFrD/attrD': [fl(i) for i in range(0x28, 0x2c)],
        'collRespCoef': fl(0x2c), 'simMethod': SIM.get(d[0x2d], hex(d[0x2d])), 'collMethod': hex(d[0x2e]),
        'collGroup': d[0x2f], 'groupsMask': [hex(x) for x in d[0x30:0x34]], 'ffMaterial': d[0x34],
        'writeData': {'numPtr': hex(pw[0]), 'bufs(pos,vel,life,dens,id,flag,cn)': [hex(x) for x in pw[1:8]],
                      'strides': pw[8:15]},
        'deletionIds': [hex(x) for x in d[0x45:0x49]], 'creationIds': [hex(x) for x in d[0x49:0x4d]],
        'packetData': [hex(x) for x in d[0x4d:0x4f]], 'flags': hex(d[0x4f]),
        'userData': hex(d[0x50]), 'name': hex(d[0x51]), 'compartment': hex(d[0x52]), 'emitters': (d[0x55] - d[0x54]) // 0x74,
    }

lines = open(sys.argv[1], errors='replace').read().splitlines()
descs, cur = [], None
for ln in lines:
    if '> NxScene::createFluid' in ln:
        cur = []
    elif cur is not None and re.search(r' desc\+[0-9A-F]{3}:', ln):
        cur += [int(x, 16) for x in ln.split(':', 1)[1].split()]
    elif cur is not None and '< NxScene::createFluid' in ln:
        descs.append((ln.split('-> ')[1], cur)); cur = None
for i, (ret, d) in enumerate(descs):
    print(f'== fluid {i} -> {ret}')
    for k, v in decode(d).items():
        print(f'  {k:34s} {v}')
summ = [l for l in lines if 'slot summary' in l]
if summ:
    k = lines.index(summ[-1])
    print('\n'.join(l.split('] ', 1)[1] for l in lines[k:k + 40] if 'Nx' in l or 'summary' in l))
