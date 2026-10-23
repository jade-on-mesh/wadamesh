#!/usr/bin/env python3
"""Resolve the merge conflicts feature branches keep hitting, by keeping both sides.

    git merge main            # stops with conflicts
    python3 scripts/resolve_conflicts.py
    git add -A && git commit

Each feature adds a line to README.md ("Word suggestions while typing are in
[PREDICTIVE_TEXT.md](PREDICTIVE_TEXT.md).") and often a host-test step to
.github/workflows/build.yml, both at the end of the same list, so two
branches always conflict there though both belong. This keeps theirs, then
ours, where every line on both sides is such a line (Markdown) or both sides
are whole workflow steps ("      - name: ..."). Any other conflict is left
alone and reported; exits non-zero if conflict markers remain.
.github/workflows/resolve-conflicts.yml runs it on open PRs when main moves.
"""
import re
import subprocess
import sys

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True,
                      check=True).stdout.strip()
HUNK = re.compile(r'<<<<<<< [^\n]*\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n', re.S)
DOC_LINE = re.compile(r'(^(\d+)\. |^[-*] |\.md\)\.?\s*$|\.md\)[^\n]*\.\s*$)')


def conflicted():
    out = subprocess.run(['git', 'diff', '--name-only', '--diff-filter=U'], cwd=ROOT, capture_output=True,
                         text=True, check=True).stdout
    return out.split()


def md_join(ours, theirs):
    lo = [l for l in ours.split('\n') if l.strip()]
    lt = [l for l in theirs.split('\n') if l.strip()]
    if not all(DOC_LINE.search(l) for l in lo + lt):
        return None
    nums = [int(m.group(1)) for l in lt if (m := re.match(r'^(\d+)\. ', l))]
    n = max(nums) if nums else None
    out = list(lt)
    for l in lo:
        if l in lt:
            continue
        m = re.match(r'^(\d+)\. ', l)
        if m and n is not None:
            n += 1
            l = f'{n}. ' + l[m.end():]
        out.append(l)
    return '\n'.join(out) + '\n'


def steps_join(ours, theirs):
    """Both sides whole steps of one job (each starting "      - name:")."""
    for side in (ours, theirs):
        first = next((l for l in side.split('\n') if l.strip()), '')
        if not re.match(r'^ {6}- name: ', first):
            return None
    if ours.strip() in theirs:
        return theirs
    return theirs.rstrip('\n') + '\n' + ours.rstrip('\n') + '\n'


def resolve(path):
    if path.endswith('.md'):
        join = md_join
    elif path.startswith('.github/workflows/') and path.endswith('.yml'):
        join = steps_join
    else:
        return False
    full = f'{ROOT}/{path}'
    s = open(full).read()
    def fix(m):
        j = join(m.group(1), m.group(2))
        return m.group(0) if j is None else j
    s = HUNK.sub(fix, s)
    open(full, 'w').write(s)
    return '<<<<<<<' not in s


def main():
    files = conflicted()
    if not files:
        print('no conflicts')
        return 0
    ok = True
    for p in files:
        good = resolve(p)
        ok &= good
        print(('resolved ' if good else 'NOT RESOLVED ') + p)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
