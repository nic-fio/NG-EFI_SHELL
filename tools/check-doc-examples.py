#!/usr/bin/env python3
"""Runs the NESH BASIC examples of the user manual with the host build and
compares their output with the output printed in the manual.

  tools/check-doc-examples.py NESH_HOST [MANUAL]

In the HTML, an example is  <pre ... data-example="NAME" ...>code</pre>  and its
output  <pre ... data-example-output="NAME">fs0:\\> COMMAND\\noutput</pre>.
All examples are written to one directory as NAME.nsb and syntax-checked
(nesh -k); the ones with an output block run in page order in that directory,
like one session. Examples marked data-platform="efi" need the firmware and
are only syntax-checked.
"""
import html
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    nesh = os.path.abspath(sys.argv[1])
    manual = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "docs" / "user-manual.html"
    text = manual.read_text()
    examples = {}
    for m in re.finditer(r'<pre[^>]*\bdata-example="([\w-]+)"[^>]*>(.*?)</pre>', text, re.S):
        examples[m.group(1)] = html.unescape(m.group(2))
    outputs = [(m.group(1), html.unescape(m.group(2)))
               for m in re.finditer(r'<pre[^>]*\bdata-example-output="([\w-]+)"[^>]*>(.*?)</pre>', text, re.S)]
    failures = 0
    with tempfile.TemporaryDirectory() as work:
        env = dict(os.environ, NESH_FS=".")
        for name, code in examples.items():
            pathlib.Path(work, name + ".nsb").write_text(code + "\n")
        r = subprocess.run([nesh, "-k"] + [n + ".nsb" for n in examples], cwd=work, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            print("syntax errors in the examples:\n" + r.stdout)
            failures += 1
        for name, block in outputs:
            first, _, expected = block.partition("\n")
            m = re.match(r"^\S*> (.*)$", first)
            if not m:
                print("%s: the output block must start with a prompt line" % name)
                failures += 1
                continue
            words = shlex.split(m.group(1))
            cmd = [nesh, words[0] + ".nsb"] + words[1:]
            r = subprocess.run(cmd, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            got = "\n".join(line.rstrip() for line in r.stdout.rstrip("\n").split("\n"))
            want = "\n".join(line.rstrip() for line in expected.rstrip("\n").split("\n"))
            if got != want:
                print("example %s: the output differs from the manual" % name)
                print("--- manual\n%s\n--- actual\n%s\n---" % (want, got))
                failures += 1
    print("manual examples: %d checked, %d with output, %d failed" % (len(examples), len(outputs), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
