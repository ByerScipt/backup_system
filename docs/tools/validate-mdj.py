#!/usr/bin/env python3
"""Validate a StarUML .mdj: unique ids, resolvable refs, parent links."""
import json
import sys

path = sys.argv[1]
data = json.load(open(path, encoding='utf-8'))

ids = {}
problems = []


def walk(o, trail):
    if isinstance(o, dict):
        if '_id' in o:
            if o['_id'] in ids:
                problems.append('duplicate _id %s at %s' % (o['_id'], trail))
            ids[o['_id']] = o
        for k, v in o.items():
            walk(v, trail + '/' + str(k))
    elif isinstance(o, list):
        for i, v in enumerate(o):
            walk(v, trail + '[%d]' % i)


walk(data, '')

refs = []


def collect(o, trail):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == '$ref' and isinstance(v, str):
                refs.append((v, trail))
            else:
                collect(v, trail + '/' + str(k))
    elif isinstance(o, list):
        for i, v in enumerate(o):
            collect(v, trail + '[%d]' % i)


collect(data, '')

for ref, trail in refs:
    if ref not in ids:
        problems.append('dangling $ref %s at %s' % (ref, trail))

for obj in ids.values():
    if obj.get('_type') == 'Project':
        continue
    parent = obj.get('_parent', {}).get('$ref')
    if parent is None:
        problems.append('element %s (%s) has no _parent' % (obj['_id'], obj.get('_type')))
    elif parent not in ids:
        problems.append('element %s parent %s missing' % (obj['_id'], parent))

# contained views must point back to their container
for obj in ids.values():
    for item in obj.get('containedViews', []):
        child = ids.get(item.get('$ref'))
        if child is None:
            continue
        back = child.get('containerView', {}).get('$ref')
        if back != obj['_id']:
            problems.append('containment mismatch: %s -> %s (back=%s)' %
                            (obj['_id'], child['_id'], back))

diagrams = [(o.get('name'), o.get('_type'), len(o.get('ownedViews', [])))
            for o in ids.values() if 'Diagram' in o.get('_type', '')]
print('elements:', len(ids), 'refs:', len(refs), 'diagrams:', len(diagrams))
for name, typ, n in diagrams:
    print('  %-46s %-22s views=%d' % (name, typ, n))
if problems:
    print('PROBLEMS (%d):' % len(problems))
    for p in problems[:40]:
        print('  ', p)
    sys.exit(1)
print('VALID')
