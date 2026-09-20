# NESH documentation

GitHub shows `.html` files as source code: it does not display web pages stored
in a repository. Read the manuals in one of these two ways.

**Online** (GitHub Pages, always the current `main`):

| Document | Link |
|---|---|
| User manual | https://nic-fio.github.io/NG-EFI_SHELL/user-manual.html |
| Developer manual | https://nic-fio.github.io/NG-EFI_SHELL/developer-manual.html |
| Start page | https://nic-fio.github.io/NG-EFI_SHELL/ |

**Offline**: clone the repository and open `docs/user-manual.html` in a browser.
Everything the pages need (style, scripts, diagrams) is in `docs/assets`, so
they work without an internet connection.

[Design decisions and history](decisions-and-history.md) is Markdown, so GitHub
shows it formatted here.

Inside NESH itself, `help` and `help NAME` print the same command reference.

## Files

| File | What |
|---|---|
| `index.html` | Start page of the documentation site. |
| `user-manual.html` | Using NESH and writing scripts (the command reference is generated from the help texts in the sources by `tools/gen-docs.py`). |
| `developer-manual.html` | Architecture, build, internals, testing, extending NESH. |
| `decisions-and-history.md` | Why NESH is the way it is. |
| `assets/` | Style sheet, scripts and a local copy of Mermaid (MIT). |
