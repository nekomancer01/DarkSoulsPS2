"""Build stamps by meaning, not bytes (ps2/proto/Makefile $(TS)/<tool>.sum, tools/area.py --sig).

A Python tool's stamp holds the hash of its syntax tree without comments and docstrings, so a comment or docstring
edit (most edits to these heavily commented tools) re-exports nothing; any other file (C sources) is hashed as bytes.
A stamp is rewritten only when that hash changes; a new stamp takes the tool's own mtime, so adding one rebuilds
nothing. An old-format stamp (sha1sum of the bytes) whose hash still matches the file is converted keeping its mtime.
<stamp>.key caches the file's size + mtime: an unchanged file is not parsed again.
Usage: srcsum.py <tool file> <stamp>"""
import ast, hashlib, os, sys

FORMAT = "ast1"


def tree_hash(src, skip=()):
    """sha1 of the module's syntax tree, docstrings dropped; skip: top-level names whose assignments are left out
    (tools/area.py's data tables, hashed per area instead)"""
    t = ast.parse(src)
    t.body = [n for n in t.body if not (isinstance(n, (ast.Assign, ast.AnnAssign)) and
                                       {getattr(x, "id", None) for x in getattr(n, "targets", [getattr(n, "target", None)])} & set(skip))]
    for n in ast.walk(t):
        b = getattr(n, "body", None)
        if isinstance(n, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and b and \
                isinstance(b[0], ast.Expr) and isinstance(b[0].value, ast.Constant) and isinstance(b[0].value.value, str):
            n.body = b[1:]
    return hashlib.sha1(ast.dump(t).encode()).hexdigest()


def file_hash(path):
    data = open(path, "rb").read()
    if path.endswith(".py"):
        try:
            return tree_hash(data)
        except SyntaxError:  # a broken tool still gets a stamp (the export itself reports the error)
            pass
    return hashlib.sha1(data).hexdigest()


def write_if_changed(path, text, first_mtime_of=None):
    """Replace path by text only when it differs (by rename: parallel makes never read half a file); a new file takes
    first_mtime_of's mtime when given. True when written."""
    try:
        if open(path).read() == text:
            return False
        new = False
    except FileNotFoundError:
        new = True
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = f"{path}.{os.getpid()}"
    with open(tmp, "w") as f:
        f.write(text)
    if new and first_mtime_of:
        st = os.stat(first_mtime_of)
        os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns))
    os.replace(tmp, path)
    return True


def stamp(tool, out):
    st = os.stat(tool)
    key = f"{st.st_size} {st.st_mtime_ns}\n"
    try:
        if open(out + ".key").read() == key and open(out).read().startswith(FORMAT):
            return
    except FileNotFoundError:
        pass
    text = f"{FORMAT} {file_hash(tool)}\n"
    try:
        old = open(out).read()
    except FileNotFoundError:
        old = None
    if old is not None and not old.startswith(FORMAT):
        # old stamp = sha1sum of the bytes: unchanged tool -> same meaning, keep the stamp's mtime
        if old.split()[0] == hashlib.sha1(open(tool, "rb").read()).hexdigest():
            tmp = f"{out}.{os.getpid()}"
            with open(tmp, "w") as f:
                f.write(text)
            os.utime(tmp, ns=(os.stat(out).st_atime_ns, os.stat(out).st_mtime_ns))
            os.replace(tmp, out)
        else:
            write_if_changed(out, text)
    else:
        write_if_changed(out, text, first_mtime_of=tool)
    write_if_changed(out + ".key", key)


if __name__ == "__main__":
    stamp(sys.argv[1], sys.argv[2])
