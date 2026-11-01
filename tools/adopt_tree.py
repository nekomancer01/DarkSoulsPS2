#!/usr/bin/env python3
"""Move day-to-day work from an older local working tree into this checkout, without changing the old tree.

Before the public repository existed, the port was developed in a separate local tree and exported here. This tool
brings over what that tree has and a clone doesn't: local, git-ignored state (config.local.mk, third-party clones in
ext/, research databases, the maintainer's private notes, job logs) is copied; source differences are only listed,
because they must land as commits.

Usage: python3 tools/adopt_tree.py OLD_TREE [--apply] [--with-build]
  (no flag)     dry run: print what would be copied and which tracked / new source files differ
  --apply       copy the local state (never overwrites anything that exists here; OLD_TREE is only read)
  --with-build  also copy build/ (exports; obj/ is left out and recompiles). Without it, run one build here.
Exit code: 0 when the sources match, 1 when some differ (copying still happens with --apply), 2 on bad usage.

Not copied, recreate instead: virtualenvs (.venv*, they hold absolute paths), lock files, __pycache__, and the
ps2/proto convenience links (make refreshes them).
"""
import filecmp
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SKIP_NAMES = {'.git', '__pycache__', '.build.lock', '.job.lock'}
SKIP_PATHS = {'ps2/proto/data', 'ps2/proto/proto.elf', 'ps2/proto/proto_prof.elf', 'ps2/proto/--help'}


def git(*args, inp=None):
    return subprocess.run(['git', '-C', ROOT] + list(args), input=inp, capture_output=True, text=True).stdout


def human(n):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return '%.1f %s' % (n, unit) if unit != 'B' else '%d B' % n
        n /= 1024.0


def size(path):
    if os.path.islink(path) or not os.path.isdir(path):
        return os.lstat(path).st_size
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(d, f)).st_size
            except OSError:
                pass
    return total


def skipped(rel, name):
    if name in SKIP_NAMES or name.endswith('.lock') or name.startswith('.venv'):
        return True
    if rel in SKIP_PATHS or (rel.startswith('ps2/proto/') and rel.count('/') == 2 and rel.endswith('.elf')):
        return True
    return False


def scan(old, with_build):
    tracked = set(git('ls-files').split('\n')) - {''}
    tracked_dirs = {os.path.dirname(p) for p in tracked}
    tracked_dirs |= {d for p in list(tracked_dirs) for d in [p.rsplit('/', i)[0] for i in range(p.count('/') + 1)]}
    copy, differs, new_src, venvs = [], [], [], []

    def ignored(rels):
        if not rels:
            return set()
        out = git('check-ignore', '--no-index', '--stdin', inp='\n'.join(rels))
        return set(out.split('\n')) - {''}

    def walk(rel_dir):
        here = os.path.join(old, rel_dir)
        try:
            names = sorted(os.listdir(here))
        except OSError:
            return
        rels = [(os.path.join(rel_dir, n) if rel_dir else n) for n in names]
        probe = [r + '/' if os.path.isdir(os.path.join(old, r)) and not os.path.islink(os.path.join(old, r))
                 else r for r in rels]
        ign = {p.rstrip('/') for p in ignored(probe)}
        for name, rel in zip(names, rels):
            src = os.path.join(old, rel)
            dst = os.path.join(ROOT, rel)
            if name.startswith('.venv'):
                venvs.append(rel)
                continue
            if skipped(rel, name):
                continue
            is_dir = os.path.isdir(src) and not os.path.islink(src)
            if rel == 'build' and not with_build:
                continue
            if rel == 'build/obj' or rel.startswith('build/obj/'):
                continue
            if rel in tracked:
                if not os.path.islink(src) and os.path.isfile(src) and not filecmp.cmp(src, dst, shallow=False):
                    differs.append(rel)
                continue
            if is_dir and (rel in tracked_dirs or rel == 'build'):
                walk(rel)  # partly tracked (or build/, to leave obj/ out): look inside
                continue
            if rel in ign or any(rel.startswith(i + '/') for i in ign):
                if not os.path.lexists(dst):
                    copy.append(rel)
                elif is_dir:
                    walk(rel)  # exists here already (e.g. an empty ext/): fill in what is missing
                continue
            if is_dir:
                walk(rel)
            elif not os.path.lexists(dst):
                new_src.append(rel)  # would show up in `git status` here: needs a commit, not a copy
    walk('')
    return copy, differs, new_src, venvs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = {a for a in sys.argv[1:] if a.startswith('--')}
    if len(args) != 1 or flags - {'--apply', '--with-build'}:
        print(__doc__)
        return 2
    old = os.path.abspath(os.path.expanduser(args[0]))
    if not os.path.isdir(old) or os.path.samefile(old, ROOT):
        print('adopt_tree: %s is not another directory' % old)
        return 2
    apply = '--apply' in flags
    copy, differs, new_src, venvs = scan(old, '--with-build' in flags)

    total = 0
    print('Local state to copy (git-ignored here, missing here):')
    for rel in copy:
        n = size(os.path.join(old, rel))
        total += n
        print('  %10s  %s%s' % (human(n), rel, '/' if os.path.isdir(os.path.join(old, rel)) else ''))
    print('  %10s  total' % human(total))
    if venvs:
        print('Virtualenvs (not copied: recreate them here, e.g. per tools/requirements*.txt):')
        for rel in venvs:
            print('  ' + rel)
    if differs:
        print('Tracked files that differ from this checkout (not copied: land them as commits):')
        for rel in differs:
            print('  ' + rel)
    if new_src:
        print('Files the old tree has that this checkout would track (not copied: commit them, or ignore them):')
        for rel in new_src:
            print('  ' + rel)

    if apply:
        for rel in copy:
            src, dst = os.path.join(old, rel), os.path.join(ROOT, rel)
            os.makedirs(os.path.dirname(dst) or ROOT, exist_ok=True)
            if os.path.islink(src):
                os.symlink(os.readlink(src), dst)
            elif os.path.isdir(src):
                shutil.copytree(src, dst, symlinks=True)
            else:
                shutil.copy2(src, dst)
        print('copied %d entries (%s); %s was not changed' % (len(copy), human(total), old))
    else:
        print('dry run: nothing copied (add --apply)')
    return 1 if differs or new_src else 0


if __name__ == '__main__':
    sys.exit(main())
