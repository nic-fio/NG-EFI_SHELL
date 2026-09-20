#!/usr/bin/env python3
"""Builds docs-preview/: ten design proposals for the manuals, with the same
content, so that the look can be chosen by comparing them side by side.

  tools/make-previews.py        writes docs-preview/theme-NN.html and index.html

Each theme is a layout (one column, left table of contents, three columns...)
plus a palette and a type scale. The content below is a slice of the user
manual, with the elements that matter: headings, tables, a script, console
output, callouts, a command card and a diagram.
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "docs-preview"

NAV = [("Introduction", "#c1"), ("Getting started", "#c2"), ("Using the command line", "#c3"),
       ("Working with files", "#c4"), ("Volumes and disks", "#c5"), ("Scripting with NESH BASIC", "#c6"),
       ("Script-friendly output", "#c7"), ("Managing boot entries", "#c8"), ("Command reference", "#c9")]

# --- the sample content, the same in every theme -------------------------

BODY = """
<h1><span class="eyebrow">User manual</span>NESH — the New EFI Shell</h1>
<p class="lead">A modern command shell for UEFI firmware: files, disks, boot entries, UEFI variables, drivers,
hardware and network, with a real scripting language built in.</p>

<h2 id="c5"><span class="num">5</span> Volumes and disks</h2>
<p>The firmware exposes two kinds of storage objects. <strong>Volumes</strong> (<code>fs0:</code>, <code>fs1:</code>…)
are file systems NESH can read and write files on. <strong>Block devices</strong> (<code>blk0</code>, <code>blk1</code>…)
are raw disks, partitions and drives, including those with no file system the firmware understands.</p>

<pre class="terminal"><span class="prompt">fs0:\\&gt; </span><span class="cmdline">map</span>
Volume Label                 Size      Free  Flags
fs0    QEMU VVFAT          503.7M    502.9M  ro boot
fs1    NESHWORK             31.9M     31.9M

Device Type            Size  Volume
blk0   removable          -  (no media)
blk1   disk            504M
blk2   partition       503M  fs0</pre>

<p>The flags say <code>ro</code> (read-only), <code>removable</code> and <code>boot</code> (the volume NESH started
from). <code>map -v</code> adds the <em>device path</em> of each entry.</p>

<table>
  <thead><tr><th>Command</th><th>What it does</th><th>Example</th></tr></thead>
  <tbody>
    <tr><td><code>map</code></td><td>Volumes and block devices</td><td><code>map -r</code></td></tr>
    <tr><td><code>vol</code></td><td>Label and free space</td><td><code>vol fs1: -n BACKUP</code></td></tr>
    <tr><td><code>dblk</code></td><td>Raw blocks of a disk</td><td><code>dblk blk1 0 1</code></td></tr>
    <tr><td><code>hexedit -d</code></td><td>Edit disk blocks</td><td><code>hexedit -d blk1 0 1</code></td></tr>
  </tbody>
</table>

<div class="callout warning"><span class="callout-title">Be careful</span>
<p>Writing raw blocks bypasses the file system: a wrong byte in a partition table can make a disk unbootable.
Save a copy of the blocks you change first.</p></div>

<h3>Finding a volume by its label</h3>
<p>Volume numbers depend on the order in which the firmware found the devices, so a script should look the volume up
by label instead:</p>

<pre class="code" data-lang="basic"><span class="c">' returns the volume with the given label, e.g. "fs1:", or "" if none</span>
<span class="k">FUNCTION</span> volume$(label$)
  <span class="k">LOCAL</span> n, i, v$(0)
  n = <span class="f">RECORDS</span>(<span class="f">RUN$</span>(<span class="s">"map -data"</span>), v$())
  <span class="k">FOR</span> i = <span class="n">0</span> <span class="k">TO</span> n - <span class="n">1</span>
    <span class="k">IF</span> <span class="f">FIELD$</span>(v$(i), <span class="s">"kind"</span>) = <span class="s">"volume"</span> <span class="k">AND</span> _
       <span class="f">UCASE$</span>(<span class="f">FIELD$</span>(v$(i), <span class="s">"label"</span>)) = <span class="f">UCASE$</span>(label$) <span class="k">THEN</span>
      <span class="k">RETURN</span> <span class="f">FIELD$</span>(v$(i), <span class="s">"volume"</span>) + <span class="s">":"</span>
    <span class="k">END IF</span>
  <span class="k">NEXT</span>
  <span class="k">RETURN</span> <span class="s">""</span>
<span class="k">END FUNCTION</span></pre>

<div class="callout tip"><span class="callout-title">Tip</span>
<p>Every command explains itself: <code>help</code> lists them and <code>help NAME</code> shows the details of one.</p></div>

<figure>
<pre class="mermaid">
flowchart LR
  D["blk1&lt;br/&gt;whole disk"] --&gt; P1["blk2&lt;br/&gt;partition 1"]
  D --&gt; P2["blk4&lt;br/&gt;partition 2"]
  P1 --&gt; F0["fs0:&lt;br/&gt;FAT file system"]
  P2 --&gt; X["no volume&lt;br/&gt;(NTFS, ext4…)"]
</pre>
<figcaption>A disk, its partitions and the volumes on them</figcaption>
</figure>

<h2 id="c9"><span class="num">9</span> Command reference</h2>
<article class="cmd">
  <h4><code>map</code> <span class="badge">-data</span></h4>
  <p class="cmd-summary">List volumes and block devices; give volumes extra names</p>
  <pre class="usage">map [-r] [-v] [-t fs|blk] | map NAME TARGET | map -d NAME</pre>
  <pre class="cmd-help">  map                 volumes (fsN:) and block devices (blkN:)
  map -r              rescan the devices (also -u)
  map usb fs1:        fs1: can also be called usb:
  map -d usb          remove an extra name
With -data: kind, volume, label, size, free, readonly, removable, boot,
aliases, devpath.</pre>
</article>
"""

# --- pieces of markup shared by the layouts ------------------------------


def toc_list(cls=""):
    items = "\n".join('      <li><a href="%s">%s</a></li>' % (h, t) for t, h in NAV)
    return '<nav class="%s"><p class="toc-title">Contents</p>\n    <ol>\n%s\n    </ol></nav>' % (cls, items)


def page(theme, css, body_html):
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s — NESH design proposal</title>
<style>
%s
</style>
</head>
<body>
<div class="ribbon">Proposal %s · <b>%s</b> — %s</div>
%s
<script src="../docs/assets/vendor/mermaid.min.js"></script>
<script>
  if (window.mermaid) {
    mermaid.initialize({ startOnLoad: true, securityLevel: "strict", theme: "base",
      themeVariables: %s });
  }
</script>
</body>
</html>
""" % (theme["name"], css, theme["n"], theme["name"], theme["idea"], body_html, theme["mermaid"])


COMMON = """
* { box-sizing: border-box; }
body { margin: 0; }
.ribbon { position: sticky; top: 0; z-index: 50; padding: .5rem 1rem; font: 500 .85rem/1.4 system-ui, sans-serif;
  background: #111827; color: #f9fafb; }
.ribbon b { color: #fff; }
img, svg { max-width: 100%; }
table { border-collapse: collapse; width: 100%; }
figure { margin: 0 0 1.5rem; }
figcaption { text-align: center; font-size: .85rem; margin-top: .5rem; }
.mermaid { text-align: center; }
pre { overflow-x: auto; }
.badge { font-size: .7rem; padding: .15rem .4rem; border-radius: .3rem; vertical-align: middle; }
.eyebrow { display: block; text-transform: uppercase; letter-spacing: .12em; font-size: .7rem; font-weight: 700; }
"""


def layout(kind, theme):
    """Wraps the sample content in one of the page layouts."""
    if kind == "left":
        return ('<div class="layout"><aside class="side"><div class="brand">%s</div>%s</aside>'
                '<main>%s</main></div>' % (theme["name"], toc_list("toc"), BODY))
    if kind == "three":
        right = ('<aside class="onthispage"><p class="toc-title">On this page</p><ol>'
                 '<li><a href="#c5">Volumes and disks</a></li>'
                 '<li><a href="#c5">Finding a volume by its label</a></li>'
                 '<li><a href="#c9">Command reference</a></li></ol></aside>')
        return ('<header class="topbar"><b>NESH</b> <span>User manual</span></header>'
                '<div class="layout"><aside class="side">%s</aside><main>%s</main>%s</div>'
                % (toc_list("toc"), BODY, right))
    if kind == "top":
        return ('<header class="topbar"><b>NESH</b><nav class="tabs">%s</nav></header>'
                '<main>%s</main>' % ("".join('<a href="%s">%s</a>' % (h, t) for t, h in NAV[:5]), BODY))
    return '<main>%s</main>' % BODY   # "single" and "book"


MERMAID_BLUE = ('{ primaryColor: "#e8f1fb", primaryTextColor: "#1c2530", primaryBorderColor: "#0a66c2",'
                ' lineColor: "#56636f", fontFamily: "system-ui, sans-serif" }')
MERMAID_WARM = ('{ primaryColor: "#f3e9d8", primaryTextColor: "#3a2f22", primaryBorderColor: "#9a6b2f",'
                ' lineColor: "#8a7355", fontFamily: "Georgia, serif" }')
MERMAID_GREY = ('{ primaryColor: "#eef1f4", primaryTextColor: "#1f2937", primaryBorderColor: "#4b5563",'
                ' lineColor: "#6b7280", fontFamily: "system-ui, sans-serif" }')
MERMAID_TEAL = ('{ primaryColor: "#e2f5f1", primaryTextColor: "#12302b", primaryBorderColor: "#0f766e",'
                ' lineColor: "#3f6b64", fontFamily: "system-ui, sans-serif" }')
MERMAID_INDIGO = ('{ primaryColor: "#eceafe", primaryTextColor: "#231f47", primaryBorderColor: "#4f46e5",'
                  ' lineColor: "#6b7280", fontFamily: "system-ui, sans-serif" }')

THEMES = []


def theme(n, name, idea, kind, css, mermaid=MERMAID_BLUE):
    THEMES.append({"n": "%02d" % n, "name": name, "idea": idea, "kind": kind, "css": css, "mermaid": mermaid})


theme(1, "Slate", "left contents, blue on white, system typeface", "left", COMMON + """
body { background: #fff; color: #1c2530; font: 16px/1.65 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
code, pre { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: .9em; }
.layout { display: grid; grid-template-columns: 17rem minmax(0, 1fr); }
.side { background: #f8fafc; border-right: 1px solid #dde3ea; padding: 1.2rem 1rem; height: 100vh;
  position: sticky; top: 2.1rem; overflow: auto; }
.brand { font-weight: 700; margin-bottom: .8rem; }
.toc-title { font-size: .72rem; letter-spacing: .1em; text-transform: uppercase; color: #6b7785; margin: 0 0 .4rem; }
.toc ol { list-style: none; margin: 0; padding: 0; font-size: .9rem; }
.toc a { display: block; padding: .25rem .5rem; border-radius: .35rem; color: #52606d; text-decoration: none; }
.toc a:hover { background: #e8f1fb; color: #0a66c2; }
main { padding: 2.5rem 3rem 5rem; max-width: 58rem; }
h1 { font-size: 2.4rem; line-height: 1.15; margin: .2rem 0 .6rem; letter-spacing: -.02em; }
.eyebrow { color: #0a66c2; margin-bottom: .3rem; }
.lead { font-size: 1.15rem; color: #56636f; max-width: 42rem; }
h2 { font-size: 1.7rem; margin: 3rem 0 1rem; padding-top: 1.2rem; border-top: 1px solid #dde3ea; }
h2 .num { color: #0a66c2; }
h3 { font-size: 1.25rem; margin: 2rem 0 .6rem; }
h4 { margin: 0 0 .2rem; }
:not(pre) > code { background: #f2f4f7; padding: .1em .35em; border-radius: .3rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { border-radius: .6rem; padding: 1rem 1.1rem; line-height: 1.55; }
pre.code { background: #f2f4f7; border: 1px solid #dde3ea; }
pre.terminal { background: #f7f9fb; border: 1px solid #dde3ea; border-left: 3px solid #94a3b8; }
.prompt { color: #0a66c2; } .cmdline { font-weight: 700; }
.k { color: #7c3aed; font-weight: 600; } .s { color: #b45309; } .n { color: #0e7490; }
.c { color: #6b7785; font-style: italic; } .f { color: #0a66c2; }
th, td { text-align: left; padding: .5rem .7rem; border-bottom: 1px solid #dde3ea; }
thead th { background: #f6f8fa; font-size: .78rem; text-transform: uppercase; letter-spacing: .05em; color: #56636f; }
.callout { border: 1px solid #dde3ea; border-left: 4px solid #0a66c2; background: #f6f8fa; border-radius: .5rem;
  padding: .8rem 1rem; margin: 0 0 1.2rem; }
.callout.tip { border-left-color: #15803d; } .callout.warning { border-left-color: #b45309; }
.callout-title { font-weight: 700; font-size: .8rem; text-transform: uppercase; letter-spacing: .06em; color: #0a66c2; }
.callout.tip .callout-title { color: #15803d; } .callout.warning .callout-title { color: #b45309; }
.cmd { border: 1px solid #dde3ea; border-radius: .7rem; padding: 1rem 1.1rem; }
.cmd-summary { color: #56636f; margin: 0 0 .6rem; }
pre.usage { background: #e8f1fb; } pre.cmd-help { background: #f2f4f7; font-size: .85rem; }
.badge { background: #e8f1fb; color: #0a66c2; font-family: ui-monospace, monospace; }
.mermaid { border: 1px solid #dde3ea; border-radius: .6rem; padding: 1rem; }
""")

theme(2, "Libro", "one column on warm paper, serif, wide margins", "book", COMMON + """
body { background: #fbf7ef; color: #33291d; font: 17px/1.75 Georgia, "Iowan Old Style", "Times New Roman", serif; }
code, pre { font-family: "SF Mono", Menlo, Consolas, monospace; font-size: .85em; }
main { max-width: 40rem; margin: 0 auto; padding: 3.5rem 1.5rem 6rem; }
h1 { font-size: 2.6rem; line-height: 1.12; margin: .4rem 0 .8rem; font-weight: 400; }
.eyebrow { color: #9a6b2f; font-family: system-ui, sans-serif; }
.lead { font-size: 1.2rem; font-style: italic; color: #6b5b46; }
h2 { font-size: 1.8rem; font-weight: 400; margin: 3.2rem 0 1rem; }
h2 .num { display: block; font-size: .75rem; letter-spacing: .2em; text-transform: uppercase; color: #9a6b2f;
  font-family: system-ui, sans-serif; }
h3 { font-size: 1.3rem; font-weight: 400; font-style: italic; margin: 2.2rem 0 .5rem; }
h4 { margin: 0 0 .3rem; }
:not(pre) > code { background: #f3ece0; padding: .1em .3em; border-radius: .2rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { background: #f5efe3; border: 1px solid #e3d8c4;
  border-radius: .25rem; padding: 1rem; line-height: 1.5; }
.prompt { color: #9a6b2f; } .cmdline { font-weight: 700; }
.k { color: #8a3f8a; } .s { color: #a35b19; } .n { color: #2a6b6b; } .c { color: #8a7355; font-style: italic; }
.f { color: #2a5b8a; }
th, td { text-align: left; padding: .45rem .6rem; border-bottom: 1px solid #e3d8c4; }
thead th { font-family: system-ui, sans-serif; font-size: .75rem; text-transform: uppercase; letter-spacing: .08em;
  color: #6b5b46; border-bottom: 2px solid #c8b48e; }
.callout { border-left: 3px solid #9a6b2f; padding: .3rem 0 .3rem 1rem; margin: 1.5rem 0; font-size: .97rem; }
.callout.tip { border-left-color: #4a7c3f; } .callout.warning { border-left-color: #b03a2e; }
.callout-title { font-family: system-ui, sans-serif; font-size: .72rem; letter-spacing: .1em; text-transform: uppercase;
  color: #9a6b2f; display: block; }
.callout.tip .callout-title { color: #4a7c3f; } .callout.warning .callout-title { color: #b03a2e; }
.cmd { border-top: 2px solid #c8b48e; padding-top: .8rem; }
.cmd-summary { font-style: italic; color: #6b5b46; }
.badge { background: #f0e4cd; color: #9a6b2f; font-family: system-ui, sans-serif; }
figcaption { font-style: italic; color: #6b5b46; }
.mermaid { background: #f5efe3; border: 1px solid #e3d8c4; padding: 1rem; }
""", MERMAID_WARM)

theme(3, "Tecnico", "dense reference manual, grey, monospace headings", "left", COMMON + """
body { background: #fff; color: #1f2937; font: 15px/1.55 system-ui, "Segoe UI", sans-serif; }
code, pre { font-family: ui-monospace, "DejaVu Sans Mono", monospace; font-size: .88em; }
.layout { display: grid; grid-template-columns: 15rem minmax(0, 1fr); }
.side { background: #f3f4f6; border-right: 2px solid #d1d5db; padding: 1rem .8rem; height: 100vh;
  position: sticky; top: 2.1rem; overflow: auto; }
.brand { font: 700 .9rem/1 ui-monospace, monospace; letter-spacing: .05em; margin-bottom: .8rem; }
.toc-title { font: 700 .68rem/1 ui-monospace, monospace; letter-spacing: .12em; color: #6b7280; margin: 0 0 .3rem; }
.toc ol { list-style: none; margin: 0; padding: 0; font-size: .82rem; counter-reset: c; }
.toc li { counter-increment: c; }
.toc a { display: block; padding: .15rem .3rem; color: #374151; text-decoration: none; }
.toc a::before { content: counter(c) ". "; color: #9ca3af; }
.toc a:hover { background: #e5e7eb; }
main { padding: 1.6rem 2rem 4rem; max-width: 62rem; }
h1 { font: 700 1.8rem/1.2 ui-monospace, monospace; margin: .2rem 0 .5rem; }
.eyebrow { color: #6b7280; font-family: ui-monospace, monospace; }
.lead { color: #4b5563; font-size: 1rem; }
h2 { font: 700 1.15rem/1.3 ui-monospace, monospace; margin: 2.2rem 0 .7rem; padding: .35rem .6rem;
  background: #374151; color: #f9fafb; border-radius: .2rem; }
h2 .num { color: #9ca3af; }
h3 { font: 700 1rem/1.3 ui-monospace, monospace; margin: 1.5rem 0 .4rem; color: #111827;
  border-bottom: 2px solid #d1d5db; padding-bottom: .2rem; }
h4 { font-family: ui-monospace, monospace; margin: 0 0 .2rem; }
:not(pre) > code { background: #eef1f4; padding: .05em .3em; border-radius: .2rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { background: #f9fafb; border: 1px solid #d1d5db; padding: .7rem .8rem;
  border-radius: .2rem; line-height: 1.45; }
pre.terminal { background: #f3f4f6; border-left: 3px solid #6b7280; }
.prompt { color: #374151; font-weight: 700; } .cmdline { font-weight: 700; }
.k { color: #6d28d9; } .s { color: #9a3412; } .n { color: #0f766e; } .c { color: #6b7280; } .f { color: #1d4ed8; }
th, td { text-align: left; padding: .3rem .55rem; border: 1px solid #d1d5db; font-size: .9rem; }
thead th { background: #e5e7eb; font-family: ui-monospace, monospace; font-size: .78rem; }
tbody tr:nth-child(even) { background: #f9fafb; }
.callout { border: 1px solid #d1d5db; border-left: 4px solid #4b5563; padding: .5rem .8rem; margin: 0 0 1rem;
  background: #f9fafb; font-size: .92rem; }
.callout.tip { border-left-color: #15803d; } .callout.warning { border-left-color: #b45309; }
.callout-title { font: 700 .7rem/1 ui-monospace, monospace; letter-spacing: .08em; color: #4b5563; display: block;
  margin-bottom: .25rem; }
.cmd { border: 1px solid #d1d5db; border-radius: .2rem; padding: .6rem .8rem; }
.cmd-summary { color: #4b5563; font-size: .92rem; margin: 0 0 .4rem; }
pre.usage { background: #eef1f4; } pre.cmd-help { font-size: .82rem; }
.badge { background: #374151; color: #f9fafb; font-family: ui-monospace, monospace; }
.mermaid { border: 1px solid #d1d5db; padding: .8rem; }
""", MERMAID_GREY)

theme(4, "Moderno", "three columns, indigo, generous spacing", "three", COMMON + """
body { background: #fff; color: #1f2333; font: 16px/1.7 "Segoe UI", system-ui, sans-serif; }
code, pre { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: .88em; }
.topbar { position: sticky; top: 2.1rem; z-index: 40; background: #fff; border-bottom: 1px solid #e7e7f0;
  padding: .8rem 2rem; font-size: .95rem; }
.topbar b { color: #4f46e5; margin-right: .5rem; }
.topbar span { color: #6b7280; }
.layout { display: grid; grid-template-columns: 16rem minmax(0, 1fr) 14rem; gap: 2rem; padding: 0 2rem; }
.side, .onthispage { position: sticky; top: 6rem; align-self: start; padding: 1.5rem 0; font-size: .9rem; }
.toc-title { font-size: .7rem; letter-spacing: .12em; text-transform: uppercase; color: #9295a8; margin: 0 0 .5rem; }
.toc ol, .onthispage ol { list-style: none; margin: 0; padding: 0; }
.toc a, .onthispage a { display: block; padding: .3rem .6rem; border-radius: .5rem; color: #52566b;
  text-decoration: none; }
.toc a:hover { background: #eceafe; color: #4f46e5; }
.onthispage { border-left: 2px solid #eceafe; padding-left: 1rem; font-size: .85rem; }
main { padding: 2.5rem 0 6rem; max-width: 48rem; }
h1 { font-size: 2.6rem; line-height: 1.1; letter-spacing: -.03em; margin: .3rem 0 .8rem; }
.eyebrow { color: #4f46e5; }
.lead { font-size: 1.2rem; color: #5b5f75; }
h2 { font-size: 1.9rem; letter-spacing: -.02em; margin: 3.5rem 0 1rem; }
h2 .num { display: inline-block; background: #eceafe; color: #4f46e5; border-radius: .5rem; padding: 0 .5rem;
  font-size: 1.1rem; vertical-align: middle; margin-right: .5rem; }
h3 { font-size: 1.3rem; margin: 2.4rem 0 .6rem; }
h4 { margin: 0 0 .3rem; }
:not(pre) > code { background: #f4f4fb; color: #4338ca; padding: .12em .4em; border-radius: .4rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { border-radius: .9rem; padding: 1.1rem 1.2rem; line-height: 1.6; }
pre.code { background: #f7f7fc; border: 1px solid #e7e7f0; }
pre.terminal { background: #fbfbfe; border: 1px solid #e7e7f0; box-shadow: inset 3px 0 0 #4f46e5; }
.prompt { color: #4f46e5; } .cmdline { font-weight: 700; }
.k { color: #7c3aed; font-weight: 600; } .s { color: #b45309; } .n { color: #0891b2; }
.c { color: #9295a8; font-style: italic; } .f { color: #2563eb; }
th, td { text-align: left; padding: .7rem .8rem; border-bottom: 1px solid #eceaf5; }
thead th { font-size: .75rem; text-transform: uppercase; letter-spacing: .08em; color: #9295a8; border-bottom: 0; }
tbody tr:hover { background: #fafaff; }
.callout { border-radius: .9rem; padding: 1rem 1.2rem; margin: 0 0 1.4rem; background: #f4f4fb; }
.callout.tip { background: #edfaf1; } .callout.warning { background: #fff6ec; }
.callout-title { font-weight: 700; font-size: .78rem; text-transform: uppercase; letter-spacing: .08em;
  color: #4f46e5; display: block; margin-bottom: .3rem; }
.callout.tip .callout-title { color: #15803d; } .callout.warning .callout-title { color: #b45309; }
.cmd { border: 1px solid #e7e7f0; border-radius: 1rem; padding: 1.2rem 1.3rem; box-shadow: 0 1px 2px rgba(31,35,51,.04); }
.cmd-summary { color: #5b5f75; margin: 0 0 .7rem; }
pre.usage { background: #eceafe; } pre.cmd-help { background: #f7f7fc; font-size: .85rem; }
.badge { background: #eceafe; color: #4f46e5; }
.mermaid { border: 1px solid #e7e7f0; border-radius: .9rem; padding: 1.2rem; }
""", MERMAID_INDIGO)

theme(5, "Contrasto", "large type, black and white, one accent, accessible", "single", COMMON + """
body { background: #fff; color: #000; font: 18px/1.7 system-ui, "Segoe UI", sans-serif; }
code, pre { font-family: ui-monospace, Consolas, monospace; font-size: .92em; }
main { max-width: 44rem; margin: 0 auto; padding: 3rem 1.5rem 6rem; }
h1 { font-size: 2.6rem; line-height: 1.1; margin: .3rem 0 .8rem; }
.eyebrow { color: #b91c1c; }
.lead { font-size: 1.25rem; }
h2 { font-size: 2rem; margin: 3rem 0 1rem; border-bottom: 4px solid #000; padding-bottom: .3rem; }
h2 .num { color: #b91c1c; }
h3 { font-size: 1.4rem; margin: 2.2rem 0 .6rem; }
h4 { margin: 0 0 .3rem; }
:not(pre) > code { background: #f0f0f0; padding: .1em .35em; border: 1px solid #d4d4d4; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { background: #f5f5f5; border: 2px solid #000; padding: 1rem;
  line-height: 1.55; }
.prompt { color: #b91c1c; font-weight: 700; } .cmdline { font-weight: 700; }
.k { font-weight: 700; } .s { color: #7c2d12; } .n { color: #134e4a; } .c { color: #525252; font-style: italic; }
.f { color: #1e3a8a; font-weight: 600; }
th, td { text-align: left; padding: .6rem .7rem; border: 2px solid #000; }
thead th { background: #000; color: #fff; }
.callout { border: 3px solid #000; padding: .9rem 1rem; margin: 0 0 1.4rem; }
.callout.warning { border-color: #b91c1c; } .callout.tip { border-color: #15803d; }
.callout-title { font-weight: 800; text-transform: uppercase; letter-spacing: .06em; display: block; }
.callout.warning .callout-title { color: #b91c1c; } .callout.tip .callout-title { color: #15803d; }
.cmd { border: 3px solid #000; padding: 1rem; }
.cmd-summary { font-weight: 600; }
pre.usage { background: #fff; border-width: 2px; } pre.cmd-help { font-size: .88rem; }
.badge { background: #000; color: #fff; }
.mermaid { border: 2px solid #000; padding: 1rem; }
a { color: #1e3a8a; }
""")

theme(6, "Editoriale", "narrow column, serif headings, notes in the margin", "book", COMMON + """
body { background: #fdfdfc; color: #22262b; font: 16.5px/1.75 "Helvetica Neue", system-ui, sans-serif; }
code, pre { font-family: "SF Mono", Menlo, Consolas, monospace; font-size: .86em; }
main { max-width: 42rem; margin: 0 auto; padding: 3rem 1.5rem 6rem; }
h1 { font: 400 3rem/1.08 "Playfair Display", Georgia, serif; margin: .2rem 0 .7rem; letter-spacing: -.01em; }
.eyebrow { color: #a3341f; }
.lead { font-size: 1.22rem; color: #50565e; border-left: 3px solid #a3341f; padding-left: 1rem; }
h2 { font: 400 2rem/1.2 Georgia, serif; margin: 3.4rem 0 1rem; }
h2 .num { color: #a3341f; font-size: 1.1rem; vertical-align: super; margin-right: .3rem; }
h3 { font: 600 1.15rem/1.3 "Helvetica Neue", sans-serif; letter-spacing: .01em; margin: 2.2rem 0 .5rem;
  text-transform: uppercase; color: #50565e; }
h4 { font-family: Georgia, serif; font-weight: 400; font-size: 1.2rem; margin: 0 0 .3rem; }
:not(pre) > code { background: #f2f1ee; padding: .1em .3em; border-radius: .2rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { background: #f7f6f3; border: 0; border-top: 2px solid #22262b;
  border-bottom: 1px solid #ddd9d2; padding: 1rem 0; line-height: 1.55; }
.prompt { color: #a3341f; } .cmdline { font-weight: 700; }
.k { color: #6b21a8; } .s { color: #a3341f; } .n { color: #0f766e; } .c { color: #8b8880; font-style: italic; }
.f { color: #1d4ed8; }
th, td { text-align: left; padding: .55rem .6rem; border-bottom: 1px solid #e5e2dc; }
thead th { font-size: .74rem; letter-spacing: .1em; text-transform: uppercase; color: #8b8880;
  border-bottom: 2px solid #22262b; }
.callout { margin: 1.6rem 0; padding: 0 0 0 1.2rem; border-left: 1px solid #ddd9d2; font-size: .95rem;
  color: #50565e; }
.callout-title { display: block; font-size: .72rem; letter-spacing: .12em; text-transform: uppercase; color: #a3341f; }
.callout.tip .callout-title { color: #166534; } .callout.warning .callout-title { color: #9a3412; }
.cmd { border-top: 2px solid #22262b; padding-top: .8rem; }
.cmd-summary { color: #50565e; font-style: italic; }
.badge { background: #f2e7e2; color: #a3341f; }
figcaption { font-style: italic; color: #8b8880; }
.mermaid { background: #f7f6f3; padding: 1.2rem; }
""", MERMAID_WARM)

theme(7, "Console", "teal accents, code blocks with a window bar", "left", COMMON + """
body { background: #fbfdfd; color: #15232b; font: 16px/1.65 system-ui, "Segoe UI", sans-serif; }
code, pre { font-family: ui-monospace, "JetBrains Mono", Menlo, monospace; font-size: .88em; }
.layout { display: grid; grid-template-columns: 16rem minmax(0, 1fr); }
.side { background: #0f766e; color: #d7f2ee; padding: 1.3rem 1rem; height: 100vh; position: sticky; top: 2.1rem;
  overflow: auto; }
.brand { font-weight: 700; color: #fff; margin-bottom: .9rem; }
.toc-title { font-size: .7rem; letter-spacing: .12em; text-transform: uppercase; color: #8fd6cd; margin: 0 0 .4rem; }
.toc ol { list-style: none; margin: 0; padding: 0; font-size: .9rem; }
.toc a { display: block; padding: .28rem .5rem; border-radius: .3rem; color: #d7f2ee; text-decoration: none; }
.toc a:hover { background: #115e56; color: #fff; }
main { padding: 2.4rem 3rem 5rem; max-width: 56rem; }
h1 { font-size: 2.3rem; line-height: 1.15; margin: .2rem 0 .6rem; }
.eyebrow { color: #0f766e; }
.lead { font-size: 1.12rem; color: #44606b; }
h2 { font-size: 1.65rem; margin: 3rem 0 1rem; color: #0b4c47; }
h2 .num { background: #0f766e; color: #fff; border-radius: 50%; display: inline-grid; place-items: center;
  width: 1.9rem; height: 1.9rem; font-size: 1rem; margin-right: .5rem; vertical-align: middle; }
h3 { font-size: 1.2rem; margin: 2rem 0 .5rem; color: #0b4c47; }
h4 { margin: 0 0 .25rem; }
:not(pre) > code { background: #e2f5f1; color: #0b4c47; padding: .1em .35em; border-radius: .3rem; }
pre.code, pre.terminal { position: relative; background: #f2faf8; border: 1px solid #bfe3dd; border-radius: .5rem;
  padding: 2rem 1rem 1rem; line-height: 1.55; }
pre.code::before, pre.terminal::before { content: "● ● ●"; position: absolute; top: .35rem; left: .8rem;
  font-size: .6rem; letter-spacing: .25em; color: #9ecfc8; }
pre.usage, pre.cmd-help { background: #f2faf8; border: 1px solid #bfe3dd; border-radius: .5rem; padding: .8rem 1rem; }
.prompt { color: #0f766e; font-weight: 700; } .cmdline { font-weight: 700; }
.k { color: #0e7490; font-weight: 600; } .s { color: #b45309; } .n { color: #7c3aed; }
.c { color: #7d9a95; font-style: italic; } .f { color: #0f766e; }
th, td { text-align: left; padding: .55rem .7rem; border-bottom: 1px solid #d7ebe7; }
thead th { background: #e2f5f1; color: #0b4c47; font-size: .78rem; text-transform: uppercase; letter-spacing: .06em; }
.callout { border-radius: .5rem; padding: .85rem 1rem; margin: 0 0 1.3rem; background: #e2f5f1;
  border-left: 4px solid #0f766e; }
.callout.tip { background: #eaf7ec; border-left-color: #15803d; }
.callout.warning { background: #fdf3e7; border-left-color: #b45309; }
.callout-title { font-weight: 700; font-size: .78rem; text-transform: uppercase; letter-spacing: .07em;
  color: #0f766e; display: block; }
.callout.tip .callout-title { color: #15803d; } .callout.warning .callout-title { color: #b45309; }
.cmd { border: 1px solid #bfe3dd; border-radius: .6rem; padding: 1rem 1.1rem; }
.cmd-summary { color: #44606b; margin: 0 0 .5rem; }
.badge { background: #0f766e; color: #fff; }
.mermaid { border: 1px solid #bfe3dd; border-radius: .5rem; padding: 1rem; }
""", MERMAID_TEAL)

theme(8, "Schede", "tabs on top, content in cards, blue and amber", "top", COMMON + """
body { background: #eef2f6; color: #1f2933; font: 16px/1.65 system-ui, "Segoe UI", sans-serif; }
code, pre { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: .88em; }
.topbar { position: sticky; top: 2.1rem; z-index: 40; background: #1e4e8c; color: #fff; padding: .7rem 2rem;
  display: flex; align-items: center; gap: 1.5rem; box-shadow: 0 2px 8px rgba(30,78,140,.25); }
.topbar b { font-size: 1.05rem; }
.tabs { display: flex; gap: .3rem; flex-wrap: wrap; }
.tabs a { color: #cfe0f5; text-decoration: none; padding: .3rem .7rem; border-radius: .4rem; font-size: .9rem; }
.tabs a:hover { background: #2a5fa3; color: #fff; }
main { max-width: 54rem; margin: 0 auto; padding: 2rem 1.5rem 5rem; }
h1 { font-size: 2.3rem; line-height: 1.15; margin: .2rem 0 .6rem; }
.eyebrow { color: #1e4e8c; }
.lead { font-size: 1.15rem; color: #52606d; }
h2 { font-size: 1.6rem; margin: 2.5rem 0 1rem; color: #1e4e8c; }
h2 .num { color: #d97706; }
h3 { font-size: 1.22rem; margin: 2rem 0 .5rem; }
h4 { margin: 0 0 .25rem; }
p, ul, table, figure, .callout, pre, article { background: transparent; }
main > p, main > .lead { max-width: 46rem; }
main > table, main > figure, main > article, main > pre, main > .callout { background: #fff; border-radius: .8rem;
  box-shadow: 0 1px 3px rgba(31,41,51,.10); }
main > table { overflow: hidden; }
main > pre { padding: 1.1rem 1.2rem; }
:not(pre) > code { background: #e3ecf7; color: #1e4e8c; padding: .1em .35em; border-radius: .3rem; }
pre.code { border-left: 4px solid #1e4e8c; }
pre.terminal { border-left: 4px solid #64748b; }
pre.usage { background: #e3ecf7; border-radius: .5rem; padding: .7rem .9rem; }
pre.cmd-help { background: #f4f7fa; border-radius: .5rem; padding: .8rem .9rem; font-size: .85rem; }
.prompt { color: #1e4e8c; } .cmdline { font-weight: 700; }
.k { color: #7c3aed; font-weight: 600; } .s { color: #b45309; } .n { color: #0e7490; }
.c { color: #7b8794; font-style: italic; } .f { color: #1e4e8c; }
th, td { text-align: left; padding: .65rem .9rem; border-bottom: 1px solid #e4e9f0; }
thead th { background: #1e4e8c; color: #fff; font-size: .78rem; text-transform: uppercase; letter-spacing: .06em; }
.callout { padding: 1rem 1.2rem; margin: 0 0 1.3rem; border-left: 5px solid #d97706; }
.callout.tip { border-left-color: #15803d; } .callout.warning { border-left-color: #b91c1c; }
.callout-title { font-weight: 700; font-size: .78rem; text-transform: uppercase; letter-spacing: .07em;
  color: #d97706; display: block; }
.callout.tip .callout-title { color: #15803d; } .callout.warning .callout-title { color: #b91c1c; }
.cmd { padding: 1.1rem 1.2rem; }
.cmd-summary { color: #52606d; margin: 0 0 .5rem; }
.badge { background: #fdf0dc; color: #b45309; }
figure { padding: 1rem; }
.mermaid { background: #fff; }
""")

theme(9, "Minimal", "one quiet column, almost no colour, lots of air", "single", COMMON + """
body { background: #fff; color: #2b2b2b; font: 17px/1.8 -apple-system, system-ui, "Segoe UI", sans-serif; }
code, pre { font-family: ui-monospace, Menlo, monospace; font-size: .85em; }
main { max-width: 38rem; margin: 0 auto; padding: 4rem 1.5rem 7rem; }
h1 { font-size: 2.1rem; font-weight: 600; line-height: 1.2; margin: .2rem 0 .7rem; letter-spacing: -.01em; }
.eyebrow { color: #8a8a8a; font-weight: 600; }
.lead { font-size: 1.1rem; color: #6a6a6a; }
h2 { font-size: 1.35rem; font-weight: 600; margin: 3.5rem 0 .8rem; }
h2 .num { color: #b0b0b0; margin-right: .3rem; }
h3 { font-size: 1.08rem; font-weight: 600; margin: 2.2rem 0 .4rem; }
h4 { font-weight: 600; margin: 0 0 .3rem; }
:not(pre) > code { background: #f4f4f4; padding: .1em .3em; border-radius: .25rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { background: #fafafa; border: 1px solid #ececec;
  border-radius: .4rem; padding: .9rem 1rem; line-height: 1.6; }
.prompt { color: #8a8a8a; } .cmdline { font-weight: 600; color: #000; }
.k { color: #444; font-weight: 600; } .s { color: #7a6a4f; } .n { color: #4f6a7a; }
.c { color: #a0a0a0; font-style: italic; } .f { color: #2b2b2b; text-decoration: underline; text-decoration-color: #ddd; }
th, td { text-align: left; padding: .5rem .2rem; border-bottom: 1px solid #ececec; }
thead th { font-size: .78rem; text-transform: uppercase; letter-spacing: .08em; color: #8a8a8a; }
.callout { padding: .2rem 0 .2rem 1rem; border-left: 2px solid #d8d8d8; margin: 1.6rem 0; color: #5a5a5a;
  font-size: .96rem; }
.callout-title { display: block; font-size: .72rem; letter-spacing: .1em; text-transform: uppercase; color: #8a8a8a; }
.cmd { border-top: 1px solid #ececec; padding-top: .9rem; }
.cmd-summary { color: #6a6a6a; }
.badge { background: #f0f0f0; color: #6a6a6a; }
figcaption { color: #8a8a8a; }
.mermaid { border: 1px solid #ececec; border-radius: .4rem; padding: 1rem; }
""", MERMAID_GREY)

theme(10, "Colonna colorata", "coloured rail on the left, strong section headers", "left", COMMON + """
body { background: #fff; color: #1b2430; font: 16px/1.68 system-ui, "Segoe UI", sans-serif; }
code, pre { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: .88em; }
.layout { display: grid; grid-template-columns: 17rem minmax(0, 1fr); }
.side { background: linear-gradient(180deg, #dbeafe 0%, #eef5ff 100%); border-right: 1px solid #c7dcf7;
  padding: 1.3rem 1.1rem; height: 100vh; position: sticky; top: 2.1rem; overflow: auto; }
.brand { font-weight: 700; color: #14427c; margin-bottom: .9rem; font-size: 1.05rem; }
.toc-title { font-size: .7rem; letter-spacing: .12em; text-transform: uppercase; color: #5b7ca6; margin: 0 0 .4rem; }
.toc ol { list-style: none; margin: 0; padding: 0; font-size: .9rem; counter-reset: c; }
.toc li { counter-increment: c; }
.toc a { display: flex; gap: .5rem; padding: .3rem .5rem; border-radius: .4rem; color: #24425f; text-decoration: none; }
.toc a::before { content: counter(c); color: #8fb3db; font-variant-numeric: tabular-nums; }
.toc a:hover { background: #fff; color: #14427c; }
main { padding: 2.5rem 3rem 5rem; max-width: 56rem; }
h1 { font-size: 2.4rem; line-height: 1.12; margin: .2rem 0 .7rem; color: #14427c; }
.eyebrow { color: #e07a1f; }
.lead { font-size: 1.15rem; color: #4a6076; }
h2 { font-size: 1.7rem; margin: 3rem 0 1rem; color: #14427c; padding-bottom: .35rem;
  border-bottom: 3px solid #e07a1f; }
h2 .num { color: #e07a1f; }
h3 { font-size: 1.22rem; margin: 2.1rem 0 .5rem; color: #1f5596; }
h4 { margin: 0 0 .25rem; }
:not(pre) > code { background: #eef5ff; color: #14427c; padding: .1em .35em; border-radius: .3rem; }
pre.code, pre.terminal, pre.usage, pre.cmd-help { border-radius: .6rem; padding: 1rem 1.1rem; line-height: 1.55; }
pre.code { background: #f6f9ff; border: 1px solid #d5e3f7; }
pre.terminal { background: #fbfcfe; border: 1px solid #d5e3f7; border-left: 4px solid #14427c; }
.prompt { color: #14427c; } .cmdline { font-weight: 700; }
.k { color: #7c3aed; font-weight: 600; } .s { color: #b45309; } .n { color: #0e7490; }
.c { color: #7f93a8; font-style: italic; } .f { color: #1f5596; }
th, td { text-align: left; padding: .6rem .8rem; border-bottom: 1px solid #e2eaf5; }
thead th { background: #14427c; color: #fff; font-size: .78rem; text-transform: uppercase; letter-spacing: .06em; }
tbody tr:nth-child(even) { background: #f8fbff; }
.callout { border-radius: .6rem; padding: .9rem 1.1rem; margin: 0 0 1.3rem; background: #eef5ff;
  border-left: 5px solid #1f5596; }
.callout.tip { background: #eefaf0; border-left-color: #15803d; }
.callout.warning { background: #fff5ea; border-left-color: #e07a1f; }
.callout-title { font-weight: 700; font-size: .78rem; text-transform: uppercase; letter-spacing: .07em;
  color: #1f5596; display: block; }
.callout.tip .callout-title { color: #15803d; } .callout.warning .callout-title { color: #c2610f; }
.cmd { border: 1px solid #d5e3f7; border-radius: .7rem; padding: 1rem 1.1rem; border-top: 4px solid #e07a1f; }
.cmd-summary { color: #4a6076; margin: 0 0 .5rem; }
pre.usage { background: #eef5ff; } pre.cmd-help { background: #f6f9ff; font-size: .85rem; }
.badge { background: #fff0e0; color: #c2610f; }
.mermaid { border: 1px solid #d5e3f7; border-radius: .6rem; padding: 1rem; }
""")


def main():
    OUT.mkdir(exist_ok=True)
    cards = []
    for t in THEMES:
        name = "theme-%s.html" % t["n"]
        (OUT / name).write_text(page(t, t["css"], layout(t["kind"], t)))
        cards.append('''  <a class="card" href="%s">
    <div class="frame"><iframe src="%s" title="%s" scrolling="no" loading="lazy"></iframe></div>
    <h3>%s. %s</h3><p>%s</p>
  </a>''' % (name, name, t["name"], t["n"], t["name"], t["idea"]))
    index = """<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NESH — dieci proposte di impaginazione</title>
<style>
  body { margin: 0; background: #f5f6f8; color: #1f2933;
    font: 16px/1.6 system-ui, "Segoe UI", sans-serif; }
  header { background: #fff; border-bottom: 1px solid #e1e5ea; padding: 2rem clamp(1rem, 4vw, 3rem); }
  h1 { margin: 0 0 .4rem; font-size: 1.8rem; }
  header p { margin: 0; color: #52606d; max-width: 46rem; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(21rem, 1fr));
    gap: 1.4rem; padding: 1.6rem clamp(1rem, 4vw, 3rem) 4rem; }
  .card { display: block; background: #fff; border: 1px solid #e1e5ea; border-radius: .8rem;
    overflow: hidden; text-decoration: none; color: inherit; box-shadow: 0 1px 3px rgba(31,41,51,.06); }
  .card:hover { border-color: #0a66c2; box-shadow: 0 6px 18px rgba(31,41,51,.12); }
  .frame { height: 15rem; overflow: hidden; border-bottom: 1px solid #e1e5ea; background: #fff; }
  iframe { width: 1400px; height: 1050px; border: 0; transform: scale(.30); transform-origin: 0 0;
    pointer-events: none; }
  .card h3 { margin: .8rem 1rem .2rem; font-size: 1.05rem; }
  .card p { margin: 0 1rem 1rem; color: #52606d; font-size: .92rem; }
</style>
</head>
<body>
<header>
  <h1>Dieci proposte di impaginazione per i manuali NESH</h1>
  <p>Stesso contenuto, dieci impianti grafici diversi: struttura della pagina, colori, tipografia e densità.
  Clicca una proposta per vederla a grandezza naturale, poi dimmi il numero che preferisci — oppure quali
  dettagli prendere da una e quali da un'altra.</p>
</header>
<div class="grid">
%s
</div>
</body>
</html>
""" % "\n".join(cards)
    (OUT / "index.html").write_text(index)
    print("%d proposals in %s" % (len(THEMES), OUT))


if __name__ == "__main__":
    main()
