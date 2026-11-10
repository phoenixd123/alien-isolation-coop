"""Per-mission and per-level audit of what co-op sync covers, from every level's scripts (no game needed).

    python -I audit.py <dir with levelmod syncscan TSVs> <repo>

Checks, for each story mission (SCRIPT_StoryMission\\Mxx...) and each level:
  - every mission composite is in worldsync.c's STORY[] list (its one-time steps are shared by the "story" rule);
  - one-time steps (LogicOnce) and saved state (persistent bools/switches) in the mission, and how many syncmap.h shares;
  - level exits (SwitchLevel), doors and locks (Door_Mechanism / Door_Package), cutscenes, the alien, other NPCs;
  - gameplay machinery whose state isn't shared by any rule or by syncmap.h (rewiring, terminals, keypads...).
Prints markdown.
"""
import sys, os, glob, re, collections

src, repo = sys.argv[1], sys.argv[2]
B = chr(92)

ws = open(os.path.join(repo, 'coopnet', 'worldsync.c'), encoding='utf-8').read()
story_block = ws[ws.index('static const uint32_t STORY[]'):]
story_block = story_block[:story_block.index('};')]
STORY = {int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{8})u', story_block)}

sm = open(os.path.join(repo, 'coopnet', 'syncmap.h'), encoding='utf-8').read()
keys_block = sm[sm.index('SYNCMAP_KEY'):sm.index('SYNCMAP_KIND[')]
keys = re.findall(r'\{0x([0-9a-f]{8})u,0x([0-9a-f]{8})u\}', keys_block)
kinds_block = sm[sm.index('SYNCMAP_KIND['):]
kinds_block = kinds_block[kinds_block.index('{') + 1:kinds_block.index('};')]
kinds = [int(x) for x in re.findall(r'\d+', kinds_block)]
SYNC = {(c, e): k for (c, e), k in zip(keys, kinds)}
CUT = set(re.findall(r'0x([0-9a-f]{8})u', sm[sm.index('CUT_COMP[CUT_COMP_N]'):sm.index('#define STORY_CAM_N')]))

ents = {}                                      # (cg, eg) -> dict
in_level = collections.defaultdict(set)        # (cg, eg) -> levels
comp_name = {}
for f in sorted(glob.glob(os.path.join(src, '*.tsv'))):
    lv = os.path.basename(f)[:-4]
    for line in open(f, encoding='utf-8', errors='replace'):
        p = line.rstrip('\n').split('\t')
        if len(p) < 7:
            continue
        cg, cn, eg, en, ty, pers, ins = p[:7]
        comp_name[cg] = cn
        e = ents.setdefault((cg, eg), {'comp': cn, 'name': en, 'type': ty, 'pers': False, 'ins': set()})
        e['pers'] |= pers == '1'
        e['ins'] |= set(x for x in ins.split(',') if x)
        in_level[(cg, eg)].add(lv)
levels = sorted({lv for s in in_level.values() for lv in s})

def mission_of(cn):
    if not cn.startswith('SCRIPT_StoryMission' + B):
        return None
    m = re.match('SCRIPT_StoryMission' + re.escape(B) + '(M[0-9]+)', cn)
    return m.group(1) if m else cn.split(B)[1]

# ---- missions
mis = collections.defaultdict(lambda: {'levels': set(), 'comps': set(), 'missing': set(), 'once': 0, 'once_sync': 0,
                                       'once_reset': 0, 'state': 0, 'state_sync': 0, 'cams': 0, 'npcs': 0})
for (cg, eg), e in ents.items():
    m = mission_of(e['comp'])
    if not m:
        continue
    d = mis[m]
    d['levels'] |= in_level[(cg, eg)]
    d['comps'].add(cg)
    if int(cg, 16) not in STORY:
        d['missing'].add(e['comp'])
    if e['type'] == 'LogicOnce':
        d['once'] += 1
        d['once_sync'] += 1   # the story rule shares every LogicOnce trigger in a STORY composite
        if 'reset' in e['ins']:
            d['once_reset'] += 1
    if e['type'] in ('VariableBool', 'LogicSwitch') and e['pers']:
        d['state'] += 1
        d['state_sync'] += 1 if SYNC.get((cg, eg)) == 1 else 0
    if e['type'] == 'StealCamera':
        d['cams'] += 1
    if e['type'] == 'Character':
        d['npcs'] += 1

print('## Story missions\n')
print('| Mission | Levels | Composites (not in STORY[]) | One-time steps shared | Saved state shared | Story cameras |')
print('|---|---|---|---|---|---|')
for m in sorted(mis, key=lambda x: (len(x), x)):
    d = mis[m]
    miss = len({c for c in d['comps'] if int(c, 16) not in STORY})
    print('| %s | %s | %d (%d) | %d/%d%s | %d/%d | %d |' % (
        m, ', '.join(sorted(d['levels'])), len(d['comps']), miss, d['once_sync'], d['once'],
        ' (%d re-armable)' % d['once_reset'] if d['once_reset'] else '', d['state_sync'], d['state'], d['cams']))
missing = sorted({n for d in mis.values() for n in d['missing']})
print('\nMission composites missing from STORY[]: %d' % len(missing))
for n in missing:
    print('- `%s`' % n)

# ---- levels
def has(lv, pred):
    return sum(1 for k, e in ents.items() if lv in in_level[k] and pred(k, e))

print('\n## Levels\n')
print('| Level | Level exits | Door_Mechanism doors | Other door archetypes | Cutscene comps | Alien | Other NPC archetypes | Shared entities (syncmap) |')
print('|---|---|---|---|---|---|---|---|')
door_other = collections.defaultdict(collections.Counter)
npc_other = collections.defaultdict(collections.Counter)
for (cg, eg), e in ents.items():
    cn = e['comp']
    for lv in in_level[(cg, eg)]:
        if e['type'] == 'composite' and 'door' in e['name'].lower() and 'Door_Mechanism' not in cn:
            pass
        if cn.startswith('AYZ' + B + 'Doors' + B) and e['type'] == 'Door':
            door_other[lv][cn.split(B)[-1]] += 1
        if cn.startswith('Archetypes' + B + 'NPCs' + B) and e['name'] == 'NPC_Logic_1':
            npc_other[lv][cn.split(B)[-1]] += 1
for lv in levels:
    exits = has(lv, lambda k, e: k[0] == 'ef3722dc' and k[1] == '92d9160a')
    doors = has(lv, lambda k, e: k[0] == '143a5b75' and e['type'] == 'LogicSwitch')
    cuts = len({k[0] for k in ents if lv in in_level[k] and k[0] in CUT})
    alien = has(lv, lambda k, e: 'Xenomorph_NPC' in e['comp'] and e['name'] == 'NPC_Logic_1')
    npcs = {n: c for n, c in npc_other[lv].items() if 'Xenomorph' not in n}
    shared = sum(1 for k in SYNC if lv in in_level.get(k, ()))
    print('| %s | %s | %s | %s | %d | %s | %s | %d |' % (
        lv, 'yes' if exits else 'NO', 'yes' if doors else 'no', len(door_other[lv]), cuts, 'yes' if alien else '-',
        ', '.join('%s' % n for n in sorted(npcs)) or '-', shared))

# ---- gameplay machinery: what's shared in each archetype
print('\n## Gameplay archetypes: their saved state and one-time steps, and how much is shared\n')
arch = collections.defaultdict(lambda: [0, 0, 0, 0, set()])
for (cg, eg), e in ents.items():
    cn = e['comp']
    if not (cn.startswith('Archetypes' + B + 'Gameplay' + B) or cn.startswith('Archetypes' + B + 'Misc_Subarchetypes' + B)
            or cn.startswith('AYZ' + B + 'Controls' + B) or cn.startswith('AYZ' + B + 'Doors' + B)):
        continue
    a = arch[cn]
    a[4] |= in_level[(cg, eg)]
    if e['type'] in ('VariableBool', 'LogicSwitch') and e['pers']:
        a[0] += 1; a[1] += SYNC.get((cg, eg)) == 1
    if e['type'] == 'LogicOnce':
        a[2] += 1; a[3] += SYNC.get((cg, eg)) == 2
print('| Archetype | Levels | Saved state shared | One-time steps shared |')
print('|---|---|---|---|')
for cn in sorted(arch, key=lambda c: -len(arch[c][4])):
    a = arch[cn]
    if a[0] or a[2]:
        print('| %s | %d | %d/%d | %d/%d |' % (cn.split(B, 1)[1], len(a[4]), a[1], a[0], a[3], a[2]))
