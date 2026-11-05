"""Repository hygiene check for PR CI (.github/workflows/pr-checks.yml, job "hygiene"); AGENTS.md sections 1 and 2.
Fails when a tracked file is, or looks like, game data or personal data, or when notes/TECH_DEBT.md reuses an ID:
  - anything under build/ or ext/, and files with game, image, video, disc or ELF extensions (.bin .pak .flver .dds
    .png .elf .iso ...; screenshots and exports belong in build/, which is git-ignored)
    The exceptions are the README screenshots (.jpg files directly in docs/screenshots/) and the README logo (README_IMAGES)
  - files over 256 KB (a large blob is almost always generated), except ALLOW_LARGE
  - home-directory paths of a real user (/home/<name>/, /Users/<name>/, C:\\Users\\<name>\\); $HOME and ~ are fine
  - the same TECH_DEBT ID twice in "## Open", twice in "## Resolved", or in both
Usage: python3 tools/ci/hygiene.py   (from anywhere; checks `git ls-files` of the repo)"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MAX_KB = 256
BANNED_DIRS = ('build/', 'ext/', 'tools/ext/')
BANNED_EXT = {
    # FromSoft / Havok / FMOD formats and exports
    '.bin', '.pak', '.flver', '.dds', '.tpf', '.bnd', '.bdt', '.bhd', '.bhd5', '.msb', '.esd', '.emevd', '.hkx',
    '.tae', '.param', '.paramdef', '.fsb', '.fev', '.ffx', '.mtd', '.dcx', '.luabnd', '.lua0', '.fmg', '.sl2', '.nvm',
    # screenshots, video, GS dumps, disc images, executables, saves
    '.png', '.jpg', '.jpeg', '.bmp', '.tga', '.gif', '.webp', '.mp4', '.mkv', '.webm', '.gs', '.gsdump', '.xz',
    '.iso', '.elf', '.exe', '.dll', '.ps2', '.mc2', '.mcd',
}
# README screenshots (AGENTS.md section 1): tracked on purpose, still within MAX_KB
SCREENSHOT_DIR = 'docs/screenshots/'
SCREENSHOT_EXT = {'.jpg'}
# README header logo, added at the maintainer's request (AGENTS.md section 1); still within MAX_KB
README_IMAGES = {'docs/logo.png'}
# Path -> why it may exceed MAX_KB (third-party, not game data)
ALLOW_LARGE = {
    'ps2/vu1test/zbyszek.c': 'ps2sdk / gsKit sample texture',
}
HOME_RE = re.compile(r'(?<![\w$])(/home/(?!runner/|user/|you/|<)[A-Za-z0-9_.-]+/|/Users/(?!<)[A-Za-z0-9_.-]+/'
                     r'|[A-Za-z]:\\\\?Users\\\\?(?!<)[A-Za-z0-9_.-]+)')


def tracked():
    out = subprocess.run(['git', 'ls-files', '-z'], cwd=ROOT, capture_output=True, check=True).stdout
    return [p for p in out.decode().split('\0') if p]


def techdebt_ids(text):
    """IDs (first column) per section of notes/TECH_DEBT.md."""
    ids = {}
    section = None
    for line in text.splitlines():
        if line.startswith('## '):
            section = line[3:].strip()
        m = re.match(r'\|\s*([A-Za-z][\w.-]*-\d+[a-z]?)\s*\|', line)
        if m and section:
            ids.setdefault(section, []).append(m.group(1))
    return ids


def main():
    errors = []
    files = tracked()
    for p in files:
        full = os.path.join(ROOT, p)
        if p.startswith(BANNED_DIRS):
            errors.append('%s: generated / third-party directory, must stay git-ignored' % p)
        ext = os.path.splitext(p)[1].lower()
        screenshot = (p.startswith(SCREENSHOT_DIR) and p.count('/') == 2 and ext in SCREENSHOT_EXT) or p in README_IMAGES
        if ext in BANNED_EXT and not screenshot:
            errors.append('%s: %s files are game data, screenshots or binaries; keep them in build/' % (p, ext))
        if not os.path.isfile(full) or os.path.islink(full):
            continue
        kb = os.path.getsize(full) // 1024
        if kb > MAX_KB and p not in ALLOW_LARGE:
            errors.append('%s: %d KB > %d KB; generated or binary data? (allowlist in tools/ci/hygiene.py)'
                          % (p, kb, MAX_KB))
        with open(full, 'rb') as f:
            data = f.read()
        if screenshot:
            continue
        if b'\0' in data[:8192]:
            errors.append('%s: binary file' % p)
            continue
        for n, line in enumerate(data.decode('utf-8', 'replace').splitlines(), 1):
            m = HOME_RE.search(line)
            if m:
                errors.append('%s:%d: personal path %r; use $HOME, ~ or a repo-relative path' % (p, n, m.group(0)))
    td = os.path.join(ROOT, 'notes', 'TECH_DEBT.md')
    if os.path.exists(td):
        ids = techdebt_ids(open(td, encoding='utf-8').read())
        seen = {}
        for section, lst in ids.items():
            for i in lst:
                if i in seen:
                    errors.append('notes/TECH_DEBT.md: ID %s used twice (%s, %s); pick a free ID' % (i, seen[i], section))
                else:
                    seen[i] = section
    for e in errors:
        print(e)
    print('hygiene: %d files checked, %d problems' % (len(files), len(errors)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
