#!/usr/bin/env python3
"""Generates the command reference of the user manual from the command tables
in the C sources, so that the manual and the built-in 'help' never disagree.

  tools/gen-docs.py           rewrite the generated part of docs/user-manual.html
  tools/gen-docs.py --check   fail if the manual is out of date, if a command has
                              no chapter below, or if a BASIC function is not
                              documented in the manual (run by 'make test')
"""
import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANUAL = ROOT / "docs" / "user-manual.html"
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
    if check:
        if new != text:
            problems.append("%s is out of date: run 'make docs'" % MANUAL.relative_to(ROOT))
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1
        print("docs: %d commands, %d BASIC functions documented" % (len(cmds), len(basic_functions())))
        return 0
    if new != text:
        MANUAL.write_text(new)
        print("updated %s" % MANUAL.relative_to(ROOT))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
