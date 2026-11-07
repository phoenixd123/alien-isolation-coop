"""Builds coopnet/syncmap.h: the level-script entities the mod shares automatically, from every level's scripts.

    1. levelmod syncscan <level>\\WORLD\\COMMANDS.PAK > <dir>\\<LEVEL>.tsv     (for every level)
    2. python -I gen_syncmap.py <dir> <repo>\\coopnet\\syncmap.h

Nothing is picked by hand. Two kinds of entity are shared, chosen by what they are:

  STATE  world state the game saves: a VariableBool or LogicSwitch marked is_persistent that scripts change
         with set_true / set_false. Cut doors, power, puzzle results, story flags. Replaying a set is harmless if
         it's already set.
  ONCE   a LogicOnce that nothing ever resets. Mission scripts and level scripts gate their one-time steps
         (story progress, cutscenes, scripted events, puzzle completions) behind these, and repeating things
         (music, lights, hints) don't use them. It fires at most once on each side, so replaying can't double up.

Left out: composites that belong to one player rather than the world (thrown items, weapons and other
Required_Assets, cameras, NPC brains, UI, hiding logic). Their effects stay with the player who did them.

It also lists where cutscenes start, for "teleport to your partner when a cutscene starts":
  CUT_COMP   the cutscene composites (Cutscenes\\..., not their Geometry\\ props): every cutscene camera animation
             in the game lives in one of these.
  STORY_CAM  StealCamera entities in story missions (the script takes your camera for a scripted moment).
"""
import sys, os, glob, collections

EXCLUDE_PREFIX = ('Required_Assets\\', 'Archetypes\\Cameras\\', 'Archetypes\\NPCs\\', 'DLC\\Archetypes\\UI\\',
                  'Level_Design\\misc_script\\ui')
EXCLUDE_WORD = ('hiding_logic',)


def excluded(comp):
    return comp.startswith(EXCLUDE_PREFIX) or any(w in comp.lower() for w in EXCLUDE_WORD)


def classify(ents):
    """(comp, eid) -> 1 STATE / 2 ONCE, by the rules in the module docstring."""
    table = {}
    for k, e in ents.items():
        if excluded(e['comp']):
            continue
        if e['type'] in ('VariableBool', 'LogicSwitch') and e['pers'] and e['ins'] & {'set_true', 'set_false'}:
            table[k] = 1
        elif e['type'] == 'LogicOnce' and 'reset' not in e['ins']:
            table[k] = 2
    return table


def selftest():
    def E(comp, ty, pers=False, ins=()):
        return {'comp': comp, 'name': '', 'type': ty, 'pers': pers, 'ins': set(ins)}
    ents = {
        ('1', 'a'): E('SCRIPT_StoryMission' + chr(92) + 'M04', 'LogicOnce', ins=['trigger']),
        ('1', 'b'): E('SCRIPT_StoryMission' + chr(92) + 'M04', 'LogicOnce', ins=['trigger', 'reset']),
        ('2', 'c'): E('Archetypes' + chr(92) + 'Misc_Subarchetypes' + chr(92) + 'Door_Mechanism', 'LogicSwitch', True, ['set_true', 'trigger']),
        ('2', 'd'): E('Archetypes' + chr(92) + 'Misc_Subarchetypes' + chr(92) + 'Door_Mechanism', 'LogicSwitch', False, ['set_false']),
        ('3', 'e'): E('Required_Assets' + chr(92) + 'Thrown' + chr(92) + 'flare', 'LogicOnce', ins=['trigger']),
        ('4', 'f'): E('Archetypes' + chr(92) + 'Misc_Subarchetypes' + chr(92) + 'Hiding_logic_shared', 'LogicOnce'),
        ('5', 'g'): E('Archetypes' + chr(92) + 'NPCs' + chr(92) + 'Alien' + chr(92) + 'Egg', 'VariableBool', True, ['set_true']),
        ('6', 'h'): E('LEVEL_Composites' + chr(92) + 'X', 'VariableBool', True, ['reference']),
        ('6', 'i'): E('LEVEL_Composites' + chr(92) + 'X', 'VariableInt', True, ['set_true']),
        ('6', 'j'): E('LEVEL_Composites' + chr(92) + 'X', 'LogicOnce'),
    }
    t = classify(ents)
    want = {('1', 'a'): 2, ('2', 'c'): 1, ('6', 'j'): 2}
    assert t == want, t
    print('gen_syncmap selftest: ok')


def main(src, out):
    ents = {}                                   # (comp, eid) -> info
    per_level = collections.defaultdict(set)
    for f in sorted(glob.glob(os.path.join(src, '*.tsv'))):
        lv = os.path.basename(f)[:-4]
        for line in open(f, encoding='utf-8', errors='replace'):
            p = line.rstrip('\n').split('\t')
            if len(p) < 7:
                continue
            cg, cn, eg, en, ty, pers, ins = p[:7]
            e = ents.setdefault((cg, eg), {'comp': cn, 'name': en, 'type': ty, 'pers': False, 'ins': set()})
            e['pers'] |= pers == '1'
            e['ins'] |= set(x for x in ins.split(',') if x)
            per_level[lv].add((cg, eg))
    table = classify(ents)
    keys = sorted(table, key=lambda k: (int(k[0], 16), int(k[1], 16)))
    B = chr(92)
    cut_comp = sorted({k[0] for k, e in ents.items() if e['comp'].startswith('Cutscenes' + B) and (B + 'Geometry' + B) not in e['comp']},
                      key=lambda c: int(c, 16))
    story_cam = sorted({k for k, e in ents.items() if e['type'] == 'StealCamera' and e['comp'].startswith('SCRIPT_StoryMission' + B)},
                       key=lambda k: (int(k[0], 16), int(k[1], 16)))
    n_state = sum(1 for k in keys if table[k] == 1)
    n_once = len(keys) - n_state
    lines = ['/* Generated by tools/syncmap/gen_syncmap.py from every level\'s scripts. Do not edit.',
             ' * The level-script entities shared automatically (see worldsync.c): composite and entity ShortGuids',
             ' * as the engine holds them, sorted. Kind 1 = STATE (persistent switch/bool), 2 = ONCE (never-reset LogicOnce).',
             ' * %d entities: %d STATE, %d ONCE, from %d levels.' % (len(keys), n_state, n_once, len(per_level)),
             ' *', ' * Per level (entities in this table that the level contains):']
    for lv in sorted(per_level):
        s = per_level[lv]
        a = sum(1 for k in s if table.get(k) == 1); b = sum(1 for k in s if table.get(k) == 2)
        lines.append(' *   %-20s %4d STATE %5d ONCE' % (lv, a, b))
    lines += [' */', '#pragma once', '#include <stdint.h>', '',
              '#define SYNCMAP_N %d' % len(keys),
              'static const uint32_t SYNCMAP_KEY[SYNCMAP_N][2] = {']
    row = []
    for k in keys:
        row.append('{0x%su,0x%su}' % k)
        if len(row) == 6:
            lines.append('    ' + ','.join(row) + ','); row = []
    if row:
        lines.append('    ' + ','.join(row) + ',')
    lines += ['};', 'static const uint8_t SYNCMAP_KIND[SYNCMAP_N] = {']
    kinds = [str(table[k]) for k in keys]
    for i in range(0, len(kinds), 40):
        lines.append('    ' + ','.join(kinds[i:i + 40]) + ',')
    lines += ['};', '',
              '/* Cutscene composites (%d) and story-mission StealCamera entities (%d). */' % (len(cut_comp), len(story_cam)),
              '#define CUT_COMP_N %d' % len(cut_comp),
              'static const uint32_t CUT_COMP[CUT_COMP_N] = {' + ','.join('0x%su' % c for c in cut_comp) + '};',
              '#define STORY_CAM_N %d' % len(story_cam),
              'static const uint32_t STORY_CAM[STORY_CAM_N][2] = {' + ','.join('{0x%su,0x%su}' % k for k in story_cam) + '};',
              'static inline int cut_is_comp(uint32_t c) { for (int i = 0; i < CUT_COMP_N; i++) if (CUT_COMP[i] == c) return 1; return 0; }',
              'static inline int cut_is_story_cam(uint32_t c, uint32_t e) {',
              '    for (int i = 0; i < STORY_CAM_N; i++) if (STORY_CAM[i][0] == c && STORY_CAM[i][1] == e) return 1;',
              '    return 0;',
              '}', '',
              '/* Kind of (composite, entity), 0 if not shared. */',
              'static inline int syncmap_kind(uint32_t comp, uint32_t eid) {',
              '    int lo = 0, hi = SYNCMAP_N - 1;',
              '    while (lo <= hi) {',
              '        int mid = (lo + hi) / 2;',
              '        uint32_t c = SYNCMAP_KEY[mid][0], e = SYNCMAP_KEY[mid][1];',
              '        if (c == comp && e == eid) return SYNCMAP_KIND[mid];',
              '        if (c < comp || (c == comp && e < eid)) lo = mid + 1; else hi = mid - 1;',
              '    }',
              '    return 0;',
              '}', '']
    open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print('%d entities (%d STATE, %d ONCE), %d cutscene composites, %d story cameras -> %s' % (len(keys), n_state, n_once, len(cut_comp), len(story_cam), out))


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        selftest()
    else:
        main(sys.argv[1], sys.argv[2])
