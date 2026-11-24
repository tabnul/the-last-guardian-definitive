#!/usr/bin/env python3
"""Guessed shader recipes for the shader list (Build shaders ahead, tlg.ini shader_prebuild): one recipe for
every shader program of the game files that no recorded recipe has (the "B" entries of the list;
the recorded ones are "A").

A recipe is the program plus the state it was translated with (user data, run-time state,
binding start numbers, the resource descriptors and vertex layout the compiler read). The game
files hold the program but not that state, so each guess takes it from a recorded recipe of
another program of the same kind (the donor), except the state that belongs to the program
itself, which the .sb file's header holds (the registers the game sets with the program):
register counts, user data count, float modes; compute: group size, group id inputs, shared
memory; pixel: the input enable and address flags, interpolated input count, depth export format
and bit, color export formats. Checked on the recorded programs: every one of these values
equals what the game recorded (color export formats: 411 of 427; the rest were depth-only
passes), except the vertex input register count, which goes with the vertex fetch the game
builds at run time and so is the donor's.
Donors, best first: a donor whose .sb header after the registers (the program's interface: its
input usage slots and input semantics) is the same; within that and else:
  1. a material shader (<template>_<vs|ps>_<hash>_m<mask>_<flags>_ex<ext>.sb): a donor of the
     same template and kind, the closest mask, flags and ext bits first (fewest differing bits);
     then one of the same template before its ';' variants;
  2. else a donor of the same folder of ps4_shader_cache and kind, the longest common start of
     the file names first;
  3. else one with the most common run-time state and binding start numbers of the kind;
  (5. a donor of the same interface that none of 1-3 names still counts, after those of 1-3.)
A vertex shader's binding start numbers are not its own: the game numbers the pixel shader's
bindings first, so they are where the paired pixel shader's end (every recorded pixel program
pairs with one start in the recorded pipeline keys). A material vertex shader's pixel shader is
the file of the same name with _ps_ for _vs_; its start comes from the keys of that pixel
shader when it was recorded, else from recorded pixel shaders of the same interface (the most
common one). A vertex shader gets a second guess (permutation 1): the next recipe in donor order
with other binding start numbers (recorded vertex shaders often have two). Ties go to the lower
program hash.

The run-time state in the guessed recipe files is the donor's; the corrected one is in the
portable file the tool writes beside them (the form make_list.py --portable reads), so build the
list from that file: make_list.py OUT_DIR GAME LIST --portable OUT_DIR/portable.bin.

A wrong guess is harmless to the picture: the game uses a cached shader only where its
specialization (the state the compiler read) equals the one of the draw (the game's own compare,
TLG_SHADER_STORE_LOOKUP); a wrong one just is not used. The import child survives a guess it
cannot build (the parent restarts it after the entry it died on).

The guessed recipes hold program bytes (game code): write them to a private folder, never into
a repository. make_list.py turns them into list entries that name the program by where the game
files hold it, as for recorded recipes.

--leave-one-out: guesses every recorded program that is in the game files from the OTHER
recorded programs (the same rule), to measure offline how often a guess equals the recording
(import both, then tlg-probe --same-permutation).

Usage: guess_recipes.py RECIPE_DIR PORTABLE GAME_DIR OUT_DIR [--leave-one-out]
       [--kinds vertex,pixel,compute] [--report FILE]
RECIPE_DIR: the recorded recipes and pipeline keys (of several sessions: merged first, see make_list.py); PORTABLE: their portable file
(tlg-probe --portable-recipes RECIPE_DIR PORTABLE). OUT_DIR must not exist or be
empty. The report (tab separated): program, kind, permutation, donor recipe, rule, distance."""
import argparse
import collections
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_list as ml  # noqa: E402

KINDS = {0: 'vertex', 1: 'pixel', 2: 'compute'}  # .sb header byte 12
HW = {'vertex': 1, 'pixel': 0, 'compute': 6}  # Shader::HwStage of the recipe
MATERIAL = re.compile(r'^(?P<t>.+?)_(?P<st>vs|ps)_[0-9a-f]{8}_m(?P<m>[0-9a-f]{8})_(?P<f>[0-9a-f]{8})'
                      r'_ex(?P<e>[0-9a-f]{8})\.sb$')
PAGE = 0x10000
MASK = (1 << 64) - 1
STAGES, KEY_HEADER = 6, 8  # Shader::SwStage::NumLogicalStages; u32 version, u32 is_compute
SW_FRAGMENT, SW_VERTEX = 0, 3  # Shader::SwStage: a graphics key's stage names, by this index


def hash_combine(seed, value):
    """common/hash.h HashCombine (u64): the stage name a pipeline key uses for program + permutation."""
    return seed ^ ((value + 0x9e3779b9 + ((seed << 12) & MASK) + (seed >> 4)) & MASK)


def key_stages(data):
    """(is compute, stage names) of a pipeline key file (vk_pipeline_serialization.cpp): a compute
    key names one stage, a graphics key six (GraphicsPipelineKey starts with stage_hashes)."""
    if len(data) < KEY_HEADER:
        raise ValueError('key too short')
    is_compute = struct.unpack_from('<I', data, 4)[0]
    count = 1 if is_compute else STAGES
    if len(data) < KEY_HEADER + 8 * count:
        raise ValueError('key too short')
    return bool(is_compute), list(struct.unpack_from('<%dQ' % count, data, KEY_HEADER))


def sb_program(data):
    """(program bytes, program hash, kind byte) of a ps4_shader_cache .sb file, or None: the code
    starts at 52 + the u32 at 60, its length is in the OrbShdr block after it (bits 8..31 of its
    second word), the program hash 16 bytes into that block."""
    if len(data) < 64:
        return None
    start = 52 + struct.unpack_from('<I', data, 60)[0]
    at = data.find(b'OrbShdr', start)
    if at < 0 or at + 24 > len(data):
        return None
    length = (struct.unpack_from('<I', data, at + 8)[0] >> 8) & 0xFFFFFF
    if length == 0 or start + length > at:
        return None
    return data[start:start + length], int.from_bytes(data[at + 16:at + 24], 'little'), data[12]


def sb_header(data, kind):
    """The registers of the program's stage (from word 17 of the .sb header: RSRC1, RSRC2, then
    vertex: VS_OUT_CONFIG, POS_FORMAT, PA_CL_VS_OUT_CNTL; pixel: Z_FORMAT, COL_FORMAT, INPUT_ENA,
    INPUT_ADDR, IN_CONTROL, BARYC_CNTL, DB_SHADER_CONTROL, CB_SHADER_MASK; compute:
    NUM_THREAD_X/Y/Z) and the header bytes after them up to the code (the program's interface)."""
    words = 10 if kind == 'pixel' else 5
    start = 52 + struct.unpack_from('<I', data, 60)[0]
    if start < 68 + 4 * words:
        return None, None
    return list(struct.unpack_from('<%dI' % words, data, 68)), data[68 + 4 * words:start]


def game_programs(game):
    """{program hash: {'kind', 'code', 'regs', 'iface', 'names': [(folder, file name)]}} of the
    game's .sb files (the update archive first, as the game reads them)."""
    programs = {}
    for name in ml.ARCHIVES:
        archive = ml.Psarc(game / name)
        for n in sorted(archive.names):
            if not (n.startswith('ps4_shader_cache/') and n.endswith('.sb')):
                continue
            data = archive.read(n)
            p = sb_program(data)
            if p is None or p[2] not in KINDS:
                continue
            code, h, kind = p
            if h not in programs:
                regs, iface = sb_header(data, KINDS[kind])
                if regs is None:
                    continue
                programs[h] = {'kind': KINDS[kind], 'code': code, 'regs': regs, 'iface': iface,
                               'names': []}
            folder, _, file = n[len('ps4_shader_cache/'):].rpartition('/')
            if (folder, file) not in programs[h]['names']:
                programs[h]['names'].append((folder, file))
        archive.close()
    return programs


def write_recipe(r, version):
    """A recipe file as shader_recipe.cpp writes it (version 1 or 2), the inverse of parse_recipe."""
    def blob(b):
        return struct.pack('<Q', len(b)) + bytes(b)
    out = struct.pack('<IIII', ml.RECIPE_MAGIC, version, len(r['rt']), len(r['start']))
    if version >= 2:
        out += struct.pack('<Q', r['layout'])
    out += struct.pack('<II', r['hw'], r['sw']) + struct.pack('<QQQ', r['hash'], r['perm'], r['base'])
    out += blob(r['ud']) + blob(r['vs']) + blob(r['flat']) + r['rt'] + r['start']
    out += struct.pack('<Q', len(r['regions']))
    for address, data in r['regions']:
        out += struct.pack('<Q', address) + blob(data)
    return out


def leb_decode(data):
    values, v, shift = [], 0, 0
    for byte in data:
        v |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            values.append(v)
            v, shift = 0, 0
    if shift:
        raise ValueError('damaged portable form')
    return values


def leb_encode(values):
    out = bytearray()
    for v in values:
        while v >= 0x80:
            out.append(v & 0x7F | 0x80)
            v >>= 7
        out.append(v)
    return bytes(out)


# Shader::RuntimeInfo in the portable form: the members in the order of its field list
# (shader_fields.h, RuntimeFields) for the stages of a guess. 0 sw_stage, 1 hw_stage, 2..8 props
# (fp_denorm_mode32, fp_denorm_mode16_64, fp_round_mode32, fp_round_mode16_64, num_user_data,
# num_input_vgprs, num_allocated_vgprs), then the stage's members. A field list of another length
# is refused (its order may have changed: update this table with shader_fields.h).
RUNTIME_FIELDS = {'vertex': 31, 'pixel': 249, 'compute': 16}
PS_EN, PS_ADDR, PS_NUM_INPUTS, PS_INPUTS, PS_COLOR, PS_Z_FORMAT, PS_MRTZ = 9, 25, 41, 42, 170, 242, 244
CS_SHARED, CS_GROUP, CS_TGID = 9, 10, 13


def own_runtime(fields, kind, regs):
    """The donor's run-time state (portable form, as numbers) with the program's own values from
    its .sb registers, as PipelineCache::BuildRuntimeInfo takes them from the same registers."""
    f = list(fields)
    rsrc1, rsrc2 = regs[0], regs[1]
    f[2], f[3] = (rsrc1 >> 16) & 3, (rsrc1 >> 18) & 3  # float denorm modes
    f[4], f[5] = (rsrc1 >> 12) & 3, (rsrc1 >> 14) & 3  # float round modes
    f[6] = (rsrc2 >> 1) & 0x1F  # user data registers
    if kind != 'vertex':  # vertex: the input registers go with the fetch the game builds
        f[7] = (rsrc1 >> 24) & 3
    f[8] = (rsrc1 & 0x3F) * 4 if kind == 'compute' else ((rsrc1 & 0x3F) + 1) * 4  # as recorded
    if kind == 'compute':
        f[CS_SHARED] = ((rsrc2 >> 15) & 0x1FF) * 512
        for i in range(3):
            f[CS_GROUP + i] = regs[2 + i] & 0xFFFF
            f[CS_TGID + i] = (rsrc2 >> (7 + i)) & 1
    elif kind == 'pixel':
        z_format, col_format, ena, addr, in_control, _, db_control = regs[2:9]
        for i in range(16):
            f[PS_EN + i], f[PS_ADDR + i] = (ena >> i) & 1, (addr >> i) & 1
        count, donor_count = in_control & 0x3F, f[PS_NUM_INPUTS]
        f[PS_NUM_INPUTS] = count
        # Inputs past the donor's: the next interpolated parameter each (the pairing with the
        # vertex shader is the game's; the most common one).
        for i in range(donor_count, min(count, 32)):
            last = f[PS_INPUTS + 4 * (i - 1)] if i else -1
            f[PS_INPUTS + 4 * i:PS_INPUTS + 4 * i + 4] = [last + 1, 0, 0, 0]
        for i in range(8):
            f[PS_COLOR + 9 * i + 3] = (col_format >> (4 * i)) & 0xF
        f[PS_Z_FORMAT] = z_format & 0xF
        f[PS_MRTZ] = (f[PS_MRTZ] & ~1) | (db_control & 1)
    return f


def read_portable(path):
    """The portable file of tlg-probe --portable-recipes: (portable id, layout id, {name: (runtime,
    bindings)}, [(key name, key)])."""
    return ml.read_portable(path)


def write_portable(path, portable_id, layout_id, recipes, keys):
    def blob(b):
        return struct.pack('<I', len(b)) + bytes(b)
    out = ml.PORTABLE_MAGIC + struct.pack('<IQQ', ml.PORTABLE_VERSION, portable_id, layout_id)
    out += struct.pack('<I', len(recipes))
    for name, (rt, start) in recipes.items():
        out += blob(name.encode()) + blob(rt) + blob(start)
    out += struct.pack('<I', len(keys))
    for name, data in keys:
        out += blob(name.encode()) + blob(data)
    path.write_bytes(out)


def popcount(v):
    return bin(v).count('1')


def materials(names):
    return [(m['t'], m['st'], int(m['m'], 16), int(m['f'], 16), int(m['e'], 16))
            for _, n in names if (m := MATERIAL.match(n))]


def common_start(a, b):
    n = 0
    while n < min(len(a), len(b)) and a[n] == b[n]:
        n += 1
    return n


class Donors:
    """The recorded programs of one kind, ranked for a target by the rule of the usage."""

    def __init__(self, recorded, game, kind):
        self.kind = kind
        self.programs = sorted(h for h, rs in recorded.items()
                               if rs[0]['hw'] == HW[kind] and h in game and game[h]['kind'] == kind)
        self.game = game
        self.mat = {h: materials(game[h]['names']) for h in self.programs}
        settings = collections.Counter((r['rt'], r['start']) for h in self.programs for r in recorded[h])
        top = settings.most_common(1)[0][0] if settings else None
        self.common = {h for h in self.programs if any((r['rt'], r['start']) == top for r in recorded[h])}

    def ranked(self, target, exclude):
        """[(donor program, rule, distance)], best first: same interface first, then the rule."""
        tm = materials(target['names'])
        ranked = []
        for h in self.programs:
            if h == exclude:
                continue
            best = None
            for t, st, m, f, e in tm:
                for t2, st2, m2, f2, e2 in self.mat[h]:
                    if st != st2:
                        continue
                    dist = popcount(m ^ m2) + popcount(f ^ f2) + popcount(e ^ e2)
                    if t == t2:
                        rule = (1, dist)
                    elif t.split(';')[0] == t2.split(';')[0]:
                        rule = (2, dist)
                    else:
                        continue
                    best = min(best or rule, rule)
            if best is None:
                for folder, n in target['names']:
                    for folder2, n2 in self.game[h]['names']:
                        if folder == folder2:
                            best = min(best or (3, 0), (3, -common_start(n, n2)))
            if best is None and h in self.common:
                best = (4, 0)
            same_iface = self.game[h]['iface'] == target['iface']
            if best is None and same_iface:
                best = (5, 0)
            if best is not None:
                ranked.append((not same_iface, *best, h))
        ranked.sort()
        return [(h, rule, dist, not differs) for differs, rule, dist, h in ranked]


class PairedStarts:
    """The binding start numbers of a vertex shader from its pixel shader (see the usage)."""

    def __init__(self, recipe_dir, recorded, portable, game):
        names = {}  # recipe stage name (program, permutation combined) -> recipe name
        for rs in recorded.values():
            for r in rs:
                names[hash_combine(r['hash'], r['perm'])] = r
        self.starts = collections.defaultdict(collections.Counter)  # pixel program -> starts
        for f in sorted(recipe_dir.glob('*.key')):
            try:
                compute, stages = key_stages(f.read_bytes())
            except ValueError:
                continue
            ps, vs = (None, None) if compute else (names.get(stages[SW_FRAGMENT]), names.get(stages[SW_VERTEX]))
            if ps and vs and vs['hw'] == HW['vertex']:
                self.starts[ps['hash']][portable[vs['name']][1]] += 1
        self.by_iface = collections.defaultdict(list)
        for h in self.starts:
            if h in game:
                self.by_iface[game[h]['iface']].append(h)
        self.by_name = collections.defaultdict(set)
        for h, p in game.items():
            for _, n in p['names']:
                self.by_name[n].add(h)
        self.game = game

    def predict(self, vertex):
        """(binding start numbers in the portable form, 'pixel recorded' | 'pixel interface'), or
        (None, None). The start is the pixel shader's (every recorded one has a single one), so
        leaving the vertex shader out (--leave-one-out) still finds it through other keys."""
        pixel = set()
        for _, n in self.game[vertex]['names']:
            if MATERIAL.match(n) and '_vs_' in n:
                pixel |= self.by_name.get(n.replace('_vs_', '_ps_', 1), set())
        known = collections.Counter()
        for ps in sorted(pixel):
            known.update(self.starts.get(ps, {}))
        if known:
            return known.most_common(1)[0][0], 'pixel recorded'
        for ps in sorted(pixel):
            for other in self.by_iface.get(self.game[ps]['iface'], []):
                known.update(self.starts[other])
        if known:
            return known.most_common(1)[0][0], 'pixel interface'
        return None, None


def guess(target, code, donor_recipe):
    """The donor's recipe with the target's program: hash, program bytes; the program moves to a
    free address when it would overlap the donor's other memory."""
    g = dict(donor_recipe)
    regions = [(a, d) for a, d in donor_recipe['regions'] if a != donor_recipe['base']]
    base = donor_recipe['base']
    if any(a < base + len(code) and base < a + len(d) for a, d in regions):
        top = max(a + len(d) for a, d in regions)
        base = (top + 2 * PAGE - 1) & ~(PAGE - 1)
    g['hash'], g['base'] = target, base
    g['regions'] = regions + [(base, code)]
    return g


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('recipes', type=pathlib.Path)
    ap.add_argument('portable', type=pathlib.Path)
    ap.add_argument('game', type=pathlib.Path)
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('--leave-one-out', action='store_true')
    ap.add_argument('--kinds', default='vertex,pixel,compute')
    ap.add_argument('--report', type=pathlib.Path)
    a = ap.parse_args()
    kinds = a.kinds.split(',')
    if not set(kinds) <= set(HW):
        sys.exit(f'--kinds: of {",".join(HW)}')
    if a.out.exists() and any(a.out.iterdir()):
        sys.exit(f'{a.out} is not empty')

    recorded, versions = {}, {}
    for f in sorted(a.recipes.glob('*.in')):
        data = f.read_bytes()
        try:
            r = ml.parse_recipe(data)
        except (ValueError, struct.error) as e:
            print(f'{f.name}: skipped ({e})')
            continue
        if write_recipe(r, struct.unpack_from('<I', data, 4)[0]) != data:
            sys.exit(f'{f.name}: the recipe writer does not give the file back byte for byte')
        versions[(r['hash'], r['perm'])] = struct.unpack_from('<I', data, 4)[0]
        r['name'] = f.stem
        recorded.setdefault(r['hash'], []).append(r)
    try:
        portable_id, layout_id, portable, _keys = read_portable(a.portable)
    except (ValueError, struct.error, UnicodeDecodeError) as e:
        sys.exit(f'{a.portable}: {e}')
    for h in list(recorded):  # only recipes the build could read (the portable file has them)
        recorded[h] = sorted((r for r in recorded[h] if r['name'] in portable), key=lambda r: r['perm'])
        if not recorded[h]:
            del recorded[h]
    game = game_programs(a.game)
    donors = {k: Donors(recorded, game, k) for k in kinds}
    vertex_start = PairedStarts(a.recipes, recorded, portable, game)

    if a.leave_one_out:
        targets = sorted(h for h, p in game.items() if h in recorded and p['kind'] in kinds
                         and recorded[h][0]['hw'] == HW[p['kind']])
    else:
        targets = sorted(h for h, p in game.items() if h not in recorded and p['kind'] in kinds)
    a.out.mkdir(parents=True, exist_ok=True)
    lines, counts, out_portable = [], collections.Counter(), {}
    for h in targets:
        p = game[h]
        ranked = donors[p['kind']].ranked(p, exclude=h)
        if not ranked:
            counts[(p['kind'], 'no donor')] += 1
            continue
        # (recipe, its rule, distance, same interface, binding start numbers or None, where from)
        best = ranked[0]
        picks = [(recorded[best[0]][0], *best[1:], None, 'donor')]
        if p['kind'] == 'vertex':
            start, source = vertex_start.predict(h)
            picks = [(recorded[best[0]][0], *best[1:], start, source)] if start else []
            seen = {start} if start else set()
            for d, rule, dist, same_iface in ranked:
                for r in recorded[d]:
                    own = portable[r['name']][1]
                    if len(picks) < 2 and own not in seen:
                        seen.add(own)
                        picks.append((r, rule, dist, same_iface, None, 'donor'))
                if len(picks) == 2:
                    break
        for perm, (r, rule, dist, same_iface, start_override, start_source) in enumerate(picks):
            g = guess(h, p['code'], r)
            g['perm'] = perm
            name = f'{h:#018x}_{perm}'
            rt, start = portable[r['name']]
            start = start_override or start
            fields = leb_decode(rt)
            if len(fields) != RUNTIME_FIELDS[p['kind']]:
                sys.exit(f'{r["name"]}: run-time state of {len(fields)} members, this tool knows '
                         f'{RUNTIME_FIELDS[p["kind"]]} (shader_fields.h changed: update RUNTIME_FIELDS)')
            out_portable[name] = (leb_encode(own_runtime(fields, p['kind'], p['regs'])), start)
            (a.out / f'{name}.in').write_bytes(write_recipe(g, versions[(r['hash'], r['perm'])]))
            lines.append(f"{h:#018x}\t{p['kind']}\t{perm}\t{r['name']}\t{rule}\t{dist}\t{int(same_iface)}"
                         f"\t{start_source}")
            counts[(p['kind'], f'binding start from {start_source}')] += 1
            counts[(p['kind'], f'rule {rule}')] += perm == 0
            counts[(p['kind'], 'same interface')] += perm == 0 and same_iface
            counts[(p['kind'], 'guesses')] += 1
    write_portable(a.out / 'portable.bin', portable_id, layout_id, out_portable, [])
    print(f'{len(targets)} programs, {len(lines)} guessed recipes written to {a.out}')
    for (kind, what), n in sorted(counts.items()):
        print(f'  {kind}: {what}: {n}')
    if a.report:
        a.report.write_text('program\tkind\tperm\tdonor\trule\tdistance\tsame_interface\tbinding_start\n'
                            + '\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
