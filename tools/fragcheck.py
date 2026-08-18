#!/usr/bin/env python3
"""Check a facility fragment for the four faults a clean parse cannot catch.

    python tools/fragcheck.py                       every fragment in clips/facility/
    python tools/fragcheck.py clips/facility/x.clip one of them

# Why this exists

Converting a standalone estate clip into a fragment of `clips/facility.clip` is mechanical, and so
are its failures. Every one of them below parses perfectly and produces a building that is wrong on
the screen and unremarkable in a material census — which is the combination that costs a session,
because the first three checks anybody reaches for all come back green.

D672 converted four buildings and D673's successor converted three more. Every fault this script
tests for was found the hard way in one of those seven, and two of them were found AFTER the clip
parsed clean, loaded, and reported a plausible rule count.

# The four

1. **A bare `paint <material>`** — no `where=`. In a standalone clip this is the base coat and it is
   correct. In a fragment it covers the WHOLE WORLD, and because later rules win and the manifest
   includes fragments in a fixed order, it repaints every building included before it. The colonnade
   and the orangery each brought one through. Both were `limestone`, and so is the manifest's own
   base coat, so the material census was identical and four buildings came back stripped.

2. **A rule naming an untranslated shape.** A fragment is drawn at its own origin and carried by one
   translate. The stone moves; a `where=`/`on=` binding does not. The rule then paints a
   building-shaped volume of air back at the model origin — and the material list still shows every
   coat at nearly the right share, because the paint did land on something.

3. **A twin standing before the shape it carries.** Gathering all the twins into one block at the
   first rule works only if every shape is bound above it. `fountain.clip` interleaves its rules
   with its definitions, so that produced 1,360 parse errors — this one is loud, and it is here so
   that the fix (each twin directly after its own source) stays fixed.

4. **A surviving whole-clip statement** — `metre`, `bounds`, `variation`, `origin`, `solid`, or a
   second `_contract.clip` include. The manifest owns all six. A second contract redefines every
   param and material; a fragment's `bounds` shrinks the box the rest of the building is sampled in.

Exit code is the number of faults, so this is a gate and not just a report.
"""

import glob
import os
import re
import sys

WHOLE_CLIP = re.compile(r'^(metre|bounds|variation|solid|origin)\s')
BARE_PAINT = re.compile(r'^paint\s+\S+\s*$')
DEFINITION = re.compile(r'^let\s+([A-Za-z0-9_]+)\s*=')
NAMED_BY_RULE = re.compile(r'\b(where|on)=([A-Za-z0-9_]+)')
# A fragment CARRIED by a translate is the only kind that owes its rules a twin.
#
# `clips/facility/` holds two sorts of file with opposite obligations. The block's own parts --
# ballroom, chapel, crypt, dome -- are drawn in facility coordinates already, so their rules name
# plain shapes and MUST NOT be twinned. The seven estate buildings are drawn at their own origin and
# carried by one translate, so every shape their rules name must be. Checking every file against the
# second rule reported 54 faults in the ballroom alone, every one of them correct code — which is
# what a checker that does not know which question it is asking produces.
CARRIED = re.compile(r'^let\s+part_[A-Za-z0-9_]+\s*=\s*translate\s*\{')

# A `#` line is prose, and these files are mostly prose.
#
# The first version of this script read comments. Every fragment's header EXPLAINS that it no longer
# includes the contract, by quoting the line it dropped -- so the checker reported the deletion as
# the fault, in two files, with the evidence of the fix as its proof of the bug. D672 warned that
# two static passes over these files disagreed and both were wrong; this made three, and it made the
# same mistake they did, which is reading text without reading what kind of text it is.
COMMENT = re.compile(r'^\s*#')

# `part_<name>` IS the translated binding -- `let part_x = translate { x_assembly } v` -- so a rule
# naming it is already carried and must NOT be asked for an `_at`. The grotto paints its tuff that
# way and it is correct.
ALREADY_CARRIED = re.compile(r'^part_[A-Za-z0-9_]+$')


def check(path):
    with open(path, encoding='utf-8', newline='') as handle:
        lines = handle.read().split('\n')

    # First definition wins: a name bound twice is a different complaint and not this script's.
    defined = {}
    for i, line in enumerate(lines):
        found = DEFINITION.match(line)
        if found and found.group(1) not in defined:
            defined[found.group(1)] = i

    carried = any(CARRIED.match(l) for l in lines)

    faults = []
    for i, line in enumerate(lines):
        if COMMENT.match(line):
            continue
        stripped = line.strip()

        if BARE_PAINT.match(stripped):
            faults.append((i, 'bare `%s` — covers the whole world and repaints every fragment '
                              'included before this one' % stripped))

        if WHOLE_CLIP.match(stripped):
            faults.append((i, 'whole-clip statement survived: `%s`' % stripped[:60]))

        if 'include "../facility/_contract.clip"' in line:
            faults.append((i, 'the contract is included again — it redefines every param and '
                              'material the manifest already set'))

        for kind, shape in NAMED_BY_RULE.findall(line):
            if not carried:
                continue        # drawn in place; its rules name plain shapes and are right to
            if ALREADY_CARRIED.match(shape):
                continue        # the translate binding itself
            if not shape.endswith('_at'):
                faults.append((i, '%s=%s names an UNTRANSLATED shape — this rule paints a '
                                  'building-shaped volume of air at the model origin' % (kind, shape)))
            elif shape not in defined:
                faults.append((i, '%s=%s is not defined anywhere' % (kind, shape)))
            elif defined[shape] > i:
                faults.append((i, '%s=%s is used at line %d but defined at line %d'
                                  % (kind, shape, i + 1, defined[shape] + 1)))

    for twin, at in sorted(defined.items(), key=lambda kv: kv[1]):
        if not carried or not twin.endswith('_at'):
            continue
        source = twin[:-3]
        if source not in defined:
            faults.append((at, 'twin `%s` carries `%s`, which is never defined' % (twin, source)))
        elif defined[source] > at:
            faults.append((at, 'twin `%s` stands at line %d, before the `%s` it carries at line %d'
                               % (twin, at + 1, source, defined[source] + 1)))

    return sorted(set(faults))


def main(argv):
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    targets = argv[1:] or sorted(glob.glob(os.path.join(root, 'clips', 'facility', '*.clip')))
    # The manifest's own pieces are not fragments of it and own none of these rules.
    targets = [t for t in targets if not os.path.basename(t).startswith('_')]

    total = 0
    for path in targets:
        faults = check(path)
        total += len(faults)
        name = os.path.relpath(path, root).replace('\\', '/')
        with open(path, encoding='utf-8', newline='') as handle:
            body = handle.read().splitlines()
        kind = 'carried' if any(CARRIED.match(l) for l in body) else 'in place'
        print('%-34s %-9s %s' % (name, kind, 'clean' if not faults else '%d FAULT(S)' % len(faults)))
        for line, why in faults:
            print('    %s:%d  %s' % (name, line + 1, why))

    print()
    print('%d fault(s) over %d file(s)' % (total, len(targets)))
    return min(total, 125)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
