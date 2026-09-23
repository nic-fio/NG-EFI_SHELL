#!/usr/bin/env python3
"""Generates from the sources the parts of the manuals that describe them: the
command reference of the user manual (from the command tables, so that the
manual and the built-in 'help' never disagree) and the line counts of the two
technical manuals, NESH's and partmgr's (so that their source maps cannot drift
away from the tree). The files of src/partmgr and tests/partmgr belong to the
partmgr manual's map, every other source file to NESH's.

  tools/gen-docs.py           rewrite the generated parts of the two manuals
  tools/gen-docs.py --check   fail if a manual is out of date, if a command has
                              no chapter below, if a source file has no row in
                              the source map, or if a BASIC function is not
                              documented in the manual (run by 'make test')
"""
import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANUAL = ROOT / "docs" / "user-manual.html"
DEVMANUAL = ROOT / "docs" / "developer-manual.html"
PMDEVMANUAL = ROOT / "docs" / "partmgr-developer-manual.html"
PARTMGR_DIRS = ("src/partmgr/", "tests/partmgr/")
BEGIN = "<!-- BEGIN GENERATED: command-reference (tools/gen-docs.py) -->"
END = "<!-- END GENERATED: command-reference -->"

# Chapter of the reference for every command. A new command must be added here:
# --check fails otherwise.
CATEGORIES = [
    ("ref-shell", "Shell and session", [
        "help", "ver", "cls", "exit", "history", "echo", "pause", "sleep", "which",
        "set", "alias", "reset", "sysinfo"]),
    ("ref-files", "Files and folders", [
        "ls", "dir", "cd", "pwd", "cat", "type", "more", "cp", "mv", "rm", "del", "mkdir", "md", "rmdir",
        "touch", "stat", "attrib", "comp", "cmp", "setsize"]),
    ("ref-text", "Text and data", [
        "grep", "head", "tail", "wc", "hexdump", "date", "time", "timezone", "parse",
        "stall", "eficompress", "efidecompress"]),
    ("ref-disks", "Volumes and disks", ["map", "vol", "dblk", "getmtc"]),
    ("ref-boot", "Boot options and UEFI variables", [
        "bootmgr", "bcfg", "var", "dmpstore", "setvar"]),
    ("ref-drivers", "Drivers and devices", [
        "drivers", "devices", "devtree", "dh", "openinfo", "connect", "disconnect", "reconnect",
        "load", "unload", "drvdiag", "drvcfg"]),
    ("ref-hw", "Hardware and firmware", [
        "memmap", "dmem", "mm", "pci", "smbiosview", "acpiview", "cpuid", "mode", "gop",
        "sermode", "loadpcirom"]),
    ("ref-net", "Network", ["ifconfig", "ifconfig6", "ping", "ping6", "tftp", "http"]),
    ("ref-editors", "Editors", ["edit", "hexedit"]),
]

MACROS = {}


def load_macros():
    for line in (ROOT / "src/core/shell.h").read_text().splitlines():
        m = re.match(r'#define\s+(\w+)\s+"([^"]*)"', line)
        if m:
            MACROS[m.group(1)] = m.group(2)


TOKEN = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[A-Za-z_]\w*|\d+|.', re.S)


def c_string(lit):
    """Decodes one C string literal (without the quotes)."""
    out, i = [], 0
    esc = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "r": "\r", "0": "\0"}
    while i < len(lit):
        c = lit[i]
        if c == "\\":
            i += 1
            if lit[i] == "x":
                m = re.match(r"[0-9a-fA-F]+", lit[i + 1:])
                out.append(chr(int(m.group(0), 16)))
                i += 1 + len(m.group(0))
                continue
            out.append(esc[lit[i]])
        else:
            out.append(c)
        i += 1
    return "".join(out)


def tokens(text):
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t.isspace() or t.startswith("//") or t.startswith("/*"):
            continue
        yield t


def table_entries(text, typename):
    """Entries of every 'static const TYPE name[] = { {...}, ... };' in text,
    each as a list of fields; adjacent string literals are joined."""
    result = []
    for m in re.finditer(r"static const %s \w+\[\] = \{" % typename, text):
        toks = list(tokens(text[m.end():text.index("};", m.end()) + 1]))
        i = 0
        while i < len(toks) and toks[i] == "{":
            i += 1
            fields, cur = [], None
            while toks[i] != "}":
                t = toks[i]
                if t.startswith('"'):
                    cur = (cur or "") + c_string(t[1:-1])
                elif t in MACROS and cur is not None:
                    cur += MACROS[t]
                elif t == ",":
                    fields.append(cur)
                    cur = None
                else:
                    cur = t if cur is None else cur + t
                i += 1
            fields.append(cur)
            result.append(fields)
            i += 1
            if i < len(toks) and toks[i] == ",":
                i += 1
    return result


def sources():
    files = sorted((ROOT / "src").rglob("*.c"))
    return [f for f in files if f.name != "host.c"]  # host.c: test-build stubs


def commands():
    cmds = {}
    for f in sources():
        for e in table_entries(f.read_text(), "Cmd"):
            name, _fn, usage, summary = e[0], e[1], e[2], e[3]
            helptext = e[4] if len(e) > 4 and e[4] != "NULL" else ""
            flags = e[5] if len(e) > 5 else "0"
            cmds[name] = {
                "name": name, "usage": usage, "summary": summary, "help": helptext or "",
                "data": "CMD_DATA" in (flags or ""), "file": str(f.relative_to(ROOT)),
            }
    return cmds


def basic_functions():
    names = set()
    for f in sources():
        for e in table_entries(f.read_text(), "BFunc"):
            names.add(e[0])
    return sorted(names)


def lines_of(path):
    with open(path, "rb") as f:
        return sum(1 for _ in f)


def source_files():
    """The files the source maps list: the headers, the C sources, the tools,
    the test programs and the partmgr tests."""
    files = set((ROOT / "include").glob("*.h"))
    files |= {p for p in (ROOT / "src").rglob("*") if p.suffix in (".c", ".h")}
    files |= {p for p in (ROOT / "tools").iterdir() if p.is_file()}
    files |= {ROOT / "tests" / "run-host-tests.sh"}
    files |= set((ROOT / "tests" / "apps").glob("*.c"))
    files |= {p for p in (ROOT / "tests" / "partmgr").iterdir() if p.is_file()}
    return {str(p.relative_to(ROOT)) for p in files}


def partmgr_file(f):
    return f.startswith(PARTMGR_DIRS)


def dir_lines(names):
    """Lines of a directory of the area table: the files it holds, and for the
    'tools, tests' row everything below them except the binary fixtures."""
    if names == ["tools", "tests"]:
        return sum(lines_of(p) for n in names for p in sorted((ROOT / n).rglob("*"))
                   if p.is_file() and "fixtures" not in p.relative_to(ROOT).parts)
    return sum(lines_of(p) for n in names for p in sorted((ROOT / n).iterdir()) if p.is_file())


# The two tables are recognised by the shape of their rows: a file in <code>
# followed by its count, and an area whose second cell names directories. Any
# other table with that shape would be rewritten too, so a numeric cell after a
# <code> cell belongs to the source map and nowhere else.
FILE_ROW = re.compile(r"(<tr><td><code>)([^<]+)(</code></td><td>)(\d+)(</td>)")
AREA_ROW = re.compile(r"(<tr><td>[^<]*</td><td>)((?:<code>[^<]+</code>(?:, )?)+)(</td><td>)([\d,]+)(</td>)")


def count_lines(manual, text, problems, required):
    """Rewrites the line counts of a technical manual: the source map row by
    row, then the totals of the area table. REQUIRED: the files its map must
    list."""
    listed = []

    def file_row(m):
        path = m.group(2)
        listed.append(path)
        if not (ROOT / path).is_file():
            problems.append("%s: the source map lists '%s', which is not a file"
                            % (manual.name, path))
            return m.group(0)
        return m.group(1) + path + m.group(3) + str(lines_of(ROOT / path)) + m.group(5)

    def area_row(m):
        names = re.findall(r"<code>([^<]+)</code>", m.group(2))
        if not all((ROOT / n).is_dir() for n in names):
            return m.group(0)                      # not the area table
        return m.group(1) + m.group(2) + m.group(3) + format(dir_lines(names), ",") + m.group(5)

    new = AREA_ROW.sub(area_row, FILE_ROW.sub(file_row, text))
    for f in sorted(required - set(listed)):
        problems.append("%s: '%s' has no row in the source map" % (manual.name, f))
    for f in sorted(set(listed)):
        if listed.count(f) > 1:
            problems.append("%s: the source map lists '%s' twice" % (manual.name, f))
        if f not in required and f in source_files():
            problems.append("%s: '%s' belongs to the source map of the other manual" % (manual.name, f))
    return new


def esc(s):
    return html.escape(s, quote=False)


def render(cmds):
    out = [BEGIN, ""]
    for cid, title, names in CATEGORIES:
        out.append('<section class="ref-group" id="%s">' % cid)
        out.append("<h3>%s</h3>" % esc(title))
        out.append('<table class="ref-summary"><thead><tr><th>Command</th><th>What it does</th></tr></thead><tbody>')
        for n in names:
            c = cmds[n]
            badge = ' <span class="badge">-data</span>' if c["data"] else ""
            out.append('<tr><td><a href="#cmd-%s"><code>%s</code></a>%s</td><td>%s</td></tr>'
                       % (n, n, badge, esc(c["summary"])))
        out.append("</tbody></table>")
        for n in names:
            c = cmds[n]
            out.append('<article class="cmd" id="cmd-%s" data-index="%s">' % (n, n))
            badge = ' <span class="badge" title="supports -data output">-data</span>' if c["data"] else ""
            out.append('<h4><code>%s</code>%s</h4>' % (n, badge))
            out.append('<p class="cmd-summary">%s</p>' % esc(c["summary"]))
            out.append('<pre class="usage">%s</pre>' % esc(c["usage"]))
            if c["help"]:
                out.append('<pre class="cmd-help">%s</pre>' % esc(c["help"].rstrip("\n")))
            out.append("</article>")
        out.append("</section>")
        out.append("")
    out.append(END)
    return "\n".join(out)


def main():
    load_macros()
    check = "--check" in sys.argv
    cmds = commands()
    problems = []
    listed = [n for _, _, names in CATEGORIES for n in names]
    for n in sorted(cmds):
        if n not in listed:
            problems.append("command '%s' (%s) has no chapter in tools/gen-docs.py" % (n, cmds[n]["file"]))
    for n in listed:
        if n not in cmds:
            problems.append("tools/gen-docs.py lists '%s', which is not a command" % n)
        if listed.count(n) > 1:
            problems.append("tools/gen-docs.py lists '%s' twice" % n)
    # 'help NAME' prints on an 80-column console
    for n, c in sorted(cmds.items()):
        for line in [c["summary"]] + c["help"].split("\n"):
            if len(line) > 78:
                problems.append("help of '%s' (%s): line longer than 78 columns: %s" % (n, c["file"], line))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    text = MANUAL.read_text()
    a, b = text.find(BEGIN), text.find(END)
    if a < 0 or b < 0:
        print("%s: generated-section markers not found" % MANUAL, file=sys.stderr)
        return 1
    new = text[:a] + render(cmds) + text[b + len(END):]
    # every BASIC function must have an entry in the function reference (<dt> of a dl.defs)
    documented = set()
    for dt in re.findall(r"<dt[^>]*>(.*?)</dt>", new, re.S):
        for part in re.sub(r"<[^>]+>", "", html.unescape(dt)).split("\u00b7"):
            m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*\$?)", part)
            if m:
                documented.add(m.group(1).lower())
    for f in basic_functions():
        if f not in documented:
            problems.append("BASIC function %s has no entry in the function reference of %s" % (f.upper(), MANUAL.name))
    files = source_files()
    devtext = DEVMANUAL.read_text()
    devnew = count_lines(DEVMANUAL, devtext, problems, {f for f in files if not partmgr_file(f)})
    pmtext = PMDEVMANUAL.read_text()
    pmnew = count_lines(PMDEVMANUAL, pmtext, problems, {f for f in files if partmgr_file(f)})
    pages = ((MANUAL, text, new), (DEVMANUAL, devtext, devnew), (PMDEVMANUAL, pmtext, pmnew))
    if check:
        for man, before, after in pages:
            if before != after:
                problems.append("%s is out of date: run 'make docs'" % man.relative_to(ROOT))
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1
        print("docs: %d commands, %d BASIC functions documented, %d source files counted"
              % (len(cmds), len(basic_functions()), len(source_files())))
        return 0
    for man, before, after in pages:
        if before != after:
            man.write_text(after)
            print("updated %s" % man.relative_to(ROOT))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
