#!/usr/bin/env python3
"""Annotate a play report (play-report.sh) in the browser: step through the screenshots, click to pin a comment on
one, and the notes are saved into the report itself (annotations/ in the zip, or in the report folder).

    python3 tools/annotate_report.py [REPORT] [--port N] [--no-open]

REPORT is a report zip or an unpacked report folder (default: the newest zip in build/play-reports/). It serves a
page on 127.0.0.1 and opens it; every change is saved into the report a moment later, so the zip can be sent as is.
What it adds to the report:
    annotations/notes.md      every annotated shot: time, note, numbered pins with their comments, the log around it
    annotations/notes.json    the same as data (pin positions as fractions of the image), read back on the next open
    annotations/t<secs>.jpg   each annotated shot with its numbered pins and comments drawn on it
Standard library only.
"""
import argparse, base64, glob, http.server, json, os, re, shutil, socketserver, subprocess, sys, tempfile, threading
import time, webbrowser, zipfile
from urllib.parse import urlparse, parse_qs, unquote

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAGE = os.path.join(HERE, 'annotate_report.html')
ANN = 'annotations/'
SHOT_RE = re.compile(r'^shots/(t(\d+(?:\.\d+)?)\.jpg)$')
LOG_T = re.compile(r'^\[\s*(\d+(?:\.\d+)?)\]')
NOISE = re.compile(r'(?i)vulkan|opengl|\bGS\b|achievements|discord|\bhost\b')


class Report:
    """A report zip or folder: read its files, and replace its annotations/ in one go."""

    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.is_zip = os.path.isfile(self.path)
        self.lock = threading.Lock()
        self._load()

    def _load(self):
        if self.is_zip:
            with zipfile.ZipFile(self.path) as z:
                self.names = z.namelist()
        else:
            self.names = [os.path.relpath(os.path.join(d, f), self.path).replace(os.sep, '/')
                          for d, _, fs in os.walk(self.path) for f in fs]

    def read(self, name):
        if name not in self.names:
            return None
        if self.is_zip:
            with zipfile.ZipFile(self.path) as z:
                return z.read(name)
        with open(os.path.join(self.path, name), 'rb') as f:
            return f.read()

    def text(self, name):
        b = self.read(name)
        return b.decode('utf-8', 'replace') if b is not None else ''

    def shots(self):
        out = []
        for n in self.names:
            m = SHOT_RE.match(n)
            if m:
                out.append({'file': m.group(1), 't': float(m.group(2))})
        return sorted(out, key=lambda s: s['t'])

    def marks(self):
        out = []
        for line in self.text('marks.txt').splitlines():
            m = re.match(r'\s*(\d+(?:\.\d+)?) s\s+mark (\d+): ?(.*)$', line)
            if m:
                out.append({'t': float(m.group(1)), 'n': int(m.group(2)), 'note': m.group(3)})
        return out

    def log_lines(self):
        if not hasattr(self, '_log'):
            self._log = []
            for line in self.text('emulog.txt').splitlines():
                m = LOG_T.match(line)
                if m and not NOISE.search(line):
                    self._log.append((float(m.group(1)), line))
        return self._log

    def log_near(self, t, before=3.0, after=1.0, cap=40):
        hits = [l for lt, l in self.log_lines() if t - before <= lt <= t + after]
        return hits[-cap:]

    def save(self, files):
        """Replace everything under annotations/ with files {name: bytes} (empty: annotations/ removed)."""
        with self.lock:
            if self.is_zip:
                d = os.path.dirname(self.path)
                fd, tmp = tempfile.mkstemp(prefix='.annot-', suffix='.zip', dir=d)
                os.close(fd)
                try:
                    with zipfile.ZipFile(self.path) as src, zipfile.ZipFile(tmp, 'w') as dst:
                        for info in src.infolist():
                            if not info.filename.startswith(ANN):
                                dst.writestr(info, src.read(info), compress_type=info.compress_type)
                        for name, data in sorted(files.items()):
                            dst.writestr(name, data, zipfile.ZIP_STORED if name.endswith('.jpg') else zipfile.ZIP_DEFLATED)
                    shutil.copymode(self.path, tmp)
                    os.replace(tmp, self.path)
                finally:
                    if os.path.exists(tmp):
                        os.unlink(tmp)
            else:
                d = os.path.join(self.path, 'annotations')
                shutil.rmtree(d, ignore_errors=True)
                for name, data in files.items():
                    p = os.path.join(self.path, name)
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    with open(p, 'wb') as f:
                        f.write(data)
            self._load()


def notes_md(report, notes):
    """annotations/notes.md: what a reader (or a thread) needs without opening the page."""
    shots = notes.get('shots', {})
    name = os.path.basename(report.path)
    out = [f'# Notes on {name}', '']
    if notes.get('summary', '').strip():
        out += [notes['summary'].strip(), '']
    if not shots:
        out += ['(no annotated shots)', '']
    marks = report.marks()
    for f in sorted(shots, key=lambda k: shots[k].get('t', 0)):
        s = shots[f]
        t = s.get('t', 0)
        out.append(f'## t = {t:.1f} s  (`shots/{f}`, drawn: `annotations/{f}`)')
        out.append('')
        near = [m for m in marks if abs(m['t'] - t) <= 3]
        for m in near:
            out.append(f'Mark {m["n"]} at {m["t"]:.1f} s: {m["note"] or "(no note)"}')
        if s.get('note', '').strip():
            out += [s['note'].strip(), '']
        elif near:
            out.append('')
        for i, p in enumerate(s.get('pins', []), 1):
            out.append(f'{i}. at x {p["x"] * 100:.0f}%, y {p["y"] * 100:.0f}% of the image: {p.get("text", "").strip() or "(no comment)"}')
        out += ['', f'![t = {t:.1f} s]({f})', '']
        log = report.log_near(t)
        if log:
            out += [f'Log from {t - 3:.1f} to {t + 1:.1f} s (emulog.txt, renderer lines left out):', '', '```']
            out += log
            out += ['```', '']
    return '\n'.join(out)


def make_handler(report):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype='application/json'):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path == '/':
                with open(PAGE, 'rb') as f:
                    return self.send(200, f.read(), 'text/html; charset=utf-8')
            if u.path == '/api/report':
                saved = report.text(ANN + 'notes.json')
                try:
                    notes = json.loads(saved) if saved else {}
                except ValueError:
                    notes = {}
                return self.send(200, json.dumps({
                    'name': os.path.basename(report.path), 'path': report.path, 'shots': report.shots(),
                    'marks': report.marks(), 'summary': report.text('summary.txt'), 'notes': notes}))
            if u.path == '/api/log':
                q = parse_qs(u.query)
                t = float(q.get('t', ['0'])[0])
                return self.send(200, json.dumps(report.log_near(t)))
            if u.path.startswith('/shots/') or u.path.startswith('/' + ANN):
                data = report.read(unquote(u.path[1:]))
                if data is None:
                    return self.send(404, 'not found', 'text/plain')
                return self.send(200, data, 'image/jpeg')
            self.send(404, 'not found', 'text/plain')

        def do_POST(self):
            if self.headers.get('Origin') not in (None, f'http://{self.headers.get("Host")}'):
                return self.send(403, 'other origin', 'text/plain')
            if urlparse(self.path).path != '/api/save':
                return self.send(404, 'not found', 'text/plain')
            try:
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
                notes = body['notes']
                shots = {f: s for f, s in notes.get('shots', {}).items() if s.get('pins') or s.get('note', '').strip()}
                notes['shots'] = shots
                files = {}
                for f in shots:
                    if not re.match(r'^t\d+(\.\d+)?\.jpg$', f):
                        raise ValueError(f'bad shot name {f}')
                    img = body.get('images', {}).get(f)
                    if img:
                        files[ANN + f] = base64.b64decode(img.split(',', 1)[1])
                    else:  # unchanged since the last save: keep the drawn one
                        old = report.read(ANN + f)
                        if old is not None:
                            files[ANN + f] = old
                if shots or notes.get('summary', '').strip():
                    files[ANN + 'notes.json'] = json.dumps(notes, indent=1).encode()
                    files[ANN + 'notes.md'] = notes_md(report, notes).encode()
                report.save(files)
                self.send(200, json.dumps({'ok': True, 'saved': time.strftime('%H:%M:%S'), 'shots': len(shots)}))
            except Exception as e:  # shown on the page
                self.send(500, json.dumps({'ok': False, 'error': f'{type(e).__name__}: {e}'}))
    return H


def open_browser(url):
    in_box = os.environ.get('CONTAINER_ID') or os.path.exists('/run/.containerenv')
    if in_box and shutil.which('distrobox-host-exec'):  # the desktop's browser, not one in the container
        if subprocess.run(['distrobox-host-exec', 'xdg-open', url], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0:
            return
    webbrowser.open(url)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('report', nargs='?', help='report zip or folder (default: newest in build/play-reports/)')
    ap.add_argument('--port', type=int, default=0, help='port on 127.0.0.1 (default: any free one)')
    ap.add_argument('--no-open', action='store_true', help='print the address, do not open a browser')
    a = ap.parse_args()
    path = a.report
    if not path:
        zips = sorted(glob.glob(os.path.join(ROOT, 'build', 'play-reports', '*.zip')), key=os.path.getmtime)
        if not zips:
            sys.exit('annotate-report: no report in build/play-reports/ (make one with ./play-report.sh, or name one)')
        path = zips[-1]
    if not os.path.exists(path):
        sys.exit(f'annotate-report: no report at {path}')
    if os.path.isfile(path) and not zipfile.is_zipfile(path):
        sys.exit(f'annotate-report: {path} is not a zip')
    report = Report(path)
    if not report.shots():
        sys.exit(f'annotate-report: {path} has no screenshots (shots/t*.jpg)')
    socketserver.TCPServer.allow_reuse_address = True
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), make_handler(report))
    url = f'http://127.0.0.1:{srv.server_address[1]}/'
    print(f'annotate-report: {report.path} ({len(report.shots())} shots)')
    print(f'annotate-report: {url}  (changes are saved into the report as you go; Ctrl+C here to stop)', flush=True)
    if not a.no_open:
        threading.Thread(target=open_browser, args=(url,), daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print()
    n = len(json.loads(report.text(ANN + 'notes.json') or '{}').get('shots', {}))
    print(f'annotate-report: {report.path}: {n} annotated shots in annotations/')


if __name__ == '__main__':
    main()
