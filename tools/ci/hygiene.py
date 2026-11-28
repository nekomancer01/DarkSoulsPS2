"""Repository hygiene check for PR CI (.github/workflows/pr-checks.yml, job "hygiene"); AGENTS.md sections 1 and 2.
Fails when a tracked file is, or looks like, game data or personal data, or when notes/TECH_DEBT.md reuses an ID:
  - anything under build/ or ext/, and files with game, image, video, disc or ELF extensions (.bin .pak .flver .dds
    .png .elf .iso ...; screenshots and exports belong in build/, which is git-ignored)
    The exceptions are the README screenshots (.jpg files directly in docs/screenshots/) and the README logo (README_IMAGES)
  - files over 256 KB (a large blob is almost always generated), except ALLOW_LARGE
  - home-directory paths of a real user (/home/<name>/, /var/home/<name>/, /Users/<name>/ also under a Windows drive
    mount /mnt/c/ or /c/, C:\\Users\\<name>\\ or C:/Users/<name>/, \\\\wsl.localhost\\<distro>\\home\\<name>); $HOME and ~
    are fine
  - the same TECH_DEBT ID twice in "## Open", twice in "## Resolved", or in both
  - mspacked (MAP_LZ4): a line of ps2/proto/src/ms*.c or mapstream.c whose code (comments left out) tests or sets
    MS_READY / MS_READING without naming MS_PACKED and without the comment "/* lz: n/a */" (a group waiting for its LZ4
    decode is memory like MS_READY and not drawable like MS_READING: every state site says which it treats it as)
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
    'notes/TECH_DEBT.md',  # hand-written debt register, not generated data
}
# Windows / WSL forms too: under WSL the Windows drive is /mnt/c/, under Git Bash /c/, Git for Windows prints C:/Users/,
# and Windows reaches WSL's files as \\wsl.localhost\<distro>\home\<name> (\\wsl$ before WSL 0.70); Fedora Atomic
# desktops (Silverblue, Bazzite) keep homes in /var/home/. Backslashes may be doubled (escaped in source strings).
HOME_RE = re.compile(r'(?<![\w$])((?:/var)?/home/(?!runner/|user/|you/|<)[A-Za-z0-9_.-]+/'
                     r'|(?:/mnt/[A-Za-z]|/[A-Za-z])?/Users/(?!<)[A-Za-z0-9_.-]+/'
                     r'|[A-Za-z]:(?:\\{1,2}|/)Users(?:\\{1,2}|/)(?!<)[A-Za-z0-9_.-]+'
                     r'|\\{2,4}wsl(?:\.localhost|\$)\\{1,2}[^\\\s]+\\{1,2}home\\{1,2}(?!<)[A-Za-z0-9_.-]+)')


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


MSPACKED_STATE = re.compile(r'\bMS_(READY|READING)\b')
MSPACKED_MARK = '/* lz: n/a */'


def code_lines(text):
    """(line number, code with comments blanked, raw line) per line of a C file."""
    in_block = False
    for n, raw in enumerate(text.splitlines(), 1):
        out, i = [], 0
        while i < len(raw):
            if in_block:
                j = raw.find('*/', i)
                if j < 0:
                    i = len(raw)
                else:
                    in_block, i = False, j + 2
            elif raw.startswith('/*', i):
                in_block, i = True, i + 2
            elif raw.startswith('//', i):
                break
            elif raw[i] == '"':  # a string literal: kept, a comment opener inside it is not one
                j = i + 1
                while j < len(raw) and raw[j] != '"':
                    j += 2 if raw[j] == '\\' else 1
                out.append(raw[i:j + 1])
                i = j + 1
            else:
                out.append(raw[i])
                i += 1
        yield n, ''.join(out), raw


def mspacked_errors(root=ROOT):
    """The mspacked check (module docstring): its problems as 'file:line: ...' strings."""
    src = os.path.join(root, 'ps2', 'proto', 'src')
    paths = sorted(p for p in os.listdir(src) if (p.startswith('ms') and p.endswith('.c')) or p == 'mapstream.c')
    errors = []
    for p in paths:
        with open(os.path.join(src, p), encoding='utf-8', errors='replace') as f:
            text = f.read()
        for n, code, raw in code_lines(text):
            if MSPACKED_STATE.search(code) and 'MS_PACKED' not in raw and MSPACKED_MARK not in raw:
                errors.append('ps2/proto/src/%s:%d: mspacked: MS_READY / MS_READING without MS_PACKED; name it or '
                              'add %s' % (p, n, MSPACKED_MARK))
    return errors


def main():
    errors = mspacked_errors()
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
