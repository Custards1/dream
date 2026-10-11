#!/usr/bin/env python3
"""Check documentation on standard-library module functions (including private helpers).

Local bindings and test/example programs are implementation details, not module API.
A signature and its implementation are one function; either may carry its docs.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
DECL = re.compile(r'(?P<private>priv\s+)?(?:virtual\s+)?let\s+(?:rec\s+)?(?P<name>[A-Za-z_][\w!]*)(?P<rest>.*)')


def advance(line, state):
    depth, quote, block = state
    i = 0
    while i < len(line):
        c, pair = line[i], line[i:i+2]
        if block:
            if pair == '/*': block += 1; i += 2; continue
            if pair == '*/': block -= 1; i += 2; continue
        elif quote:
            if c == '\\': i += 2; continue
            if c == quote: quote = ''
        elif pair == '//': break
        elif pair == '/*': block = 1; i += 2; continue
        elif c in '\"\'': quote = c
        elif c in '{[(': depth += 1
        elif c in '}])': depth -= 1
        i += 1
    return depth, quote, block


def functions(path):
    state = (0, '', 0)
    comments = []
    scopes = []
    entries = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        s = line.strip()
        level = scopes[-1][0] if scopes else 0
        at_module = state == (level, '', 0)
        if at_module and s.startswith('///'):
            comments.append(s[3:].strip())
            continue
        match = DECL.fullmatch(s) if at_module else None
        if match:
            name = '.'.join([scope[1] for scope in scopes] + [match['name']])
            rest = match['rest'].strip()
            entry = entries.setdefault(name, {'line': number, 'function': False, 'documented': False})
            # Function signatures, implementations with arguments, and aliases
            # with function signatures are all covered. Constants aren't functions.
            entry['function'] |= ('->' in rest and rest.startswith(':')) or bool(re.match(r'[^:=]+=', rest))
            entry['documented'] |= bool(''.join(comments))
        if s:
            comments = []
        module = re.match(r'(?:priv\s+)?mod\s+(\w+)\s*\{', s) if at_module else None
        state = advance(line, state)
        if module:
            scopes.append((level + 1, module[1]))
        while scopes and state[0] < scopes[-1][0]:
            scopes.pop()
    return {name: entry for name, entry in entries.items() if entry['function']}


def check():
    total, missing = 0, []
    for path in sorted((ROOT / 'mind/std').rglob('*.dr')):
        if {'tests', 'examples'} & set(path.parts) or path.name == 'tests.dr':
            continue
        for name, entry in functions(path).items():
            total += 1
            if not entry['documented']:
                missing.append(f"{path.relative_to(ROOT)}:{entry['line']}: {name} needs a /// comment")
    if missing:
        print('\n'.join(missing))
        return 1
    print(f'Standard library: {total} module functions documented')
    return 0


if __name__ == '__main__':
    sys.exit(check())
