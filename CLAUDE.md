# Working on NESH

How this project is developed: the agreements, the decisions that are settled,
and what to check before committing. A fresh clone plus this file is the whole
working context — nothing needed to continue the work lives outside the
repository.

## How we work

- **Talk to the user in Italian.** The code, the comments and the documentation
  are in English; the conversation is not.
- **Agree the design before writing code.** On a new area, propose the options
  and wait for a decision. The user stopped an early coding session precisely to
  do this.
- **One decision at a time, in plain language.** Long lists of simultaneous
  questions do not work here.
- **Verify, do not claim.** Every statement about behaviour comes from a run:
  the host tests, QEMU, a screenshot. "It should work" is not a result.
- **The documentation matters as much as the code** (user: *"un progetto serio
  dà alla documentazione la stessa importanza del codice"*). A feature is not
  done until the manual describes it.

## Settled decisions — do not reopen them

These were decided with the user and should not be proposed again unless the
user brings them up. The reasoning for each is in
[docs/decisions-and-history.md](docs/decisions-and-history.md).

| Decision | |
|---|---|
| **One `.efi` file** | Everything is built in: one signature for Secure Boot, nothing to install. |
| **BASIC-like scripting**, not a bash clone | The user chose it; commands are called from scripts with `RUN` / `RUN$()`, implicit `RUN` only at the prompt. |
| **No EDK2 code** | Every command NESH lacks is rewritten from scratch; EDK2 option letters are accepted where they do not clash. |
| **`-data` instead of `-sfo`** | `key=value` records read by `RECORDS()` / `FIELD$()`; `-sfo` errors and points at `-data`. |
| **`msr` stays out** | Reading MSRs hangs with #GP on the firmware; the user decided against it. |
| **No legacy EDK1 shell interface** | User: *"lasciamo riposare in pace il vecchiume"*. |
| **Secure Boot: reads yes, writes no** | Low-level hardware writes (`mm` on I/O and PCI config, `hexedit -m`) are refused while Secure Boot is active. The **clock and the serial port stay allowed** — the user ruled they are not a security risk. |
| **Partition management is `partmgr.efi`**, not a NESH command | A separate full-screen utility in this repository, same release; `nesh.efi` stays one file. Designed in D21, not built yet. |
| **Light documentation only** | No dark themes, no dark-mode media queries, no theme toggle, no dark code blocks. |
| **Apache 2.0 + Commons Clause** | Free to use and share, selling it needs a commercial licence, asked for by opening an issue. Not OSI open source, and the project says so up front. |

## The repository

| Where | What |
|---|---|
| `src/core`, `src/basic`, `src/cmd` | Shell core, the BASIC interpreter, the commands. |
| `src/pal`, `src/platform` | The platform layer: `pal_efi.c` for firmware, `pal_host.c` for the Linux test build. |
| `tools/` | `elf2efi.py` (ELF &rarr; PE32+), `gen-docs.py`, `check-doc-examples.py`, `run-qemu.sh`, `mkusb.sh`, `record-demo.py`, `setup-dev.sh`. |
| `tests/` | Host tests, QEMU scripts (`tests/efi/*.nsb`) and binary fixtures (EDK2 applications, network drivers). |
| `docs/` | The two HTML manuals, the decisions log, the assets. Published with GitHub Pages. |
| `examples/` | The `.nsb` scripts shipped with NESH and shown in the manual. |
| `build/` | Products only, never committed. |

## Before committing

1. `make test` — host tests, and the documentation checks: every command and
   every BASIC function must be documented, help lines stay within 78 columns,
   and the examples in the manual are run and compared against their printed
   output. **This fails if the manual is out of date**; `make docs` regenerates
   the command reference from the help texts in the C sources, and the line
   counts of the developer manual from the files themselves (a new source file
   needs its row in the source map).
2. `make qemu-test` when anything on the firmware side changed, and
   `make qemu-nettest` / `make qemu-sbtest` for network or Secure Boot work.
3. Do not report success from a pipeline that hid a failure: check the exit
   status, and in the QEMU logs the run is only a pass with `failures:0`.
4. Commits use the repository-local identity set by `tools/setup-dev.sh`, which
   keeps the personal address out of the public history.

Releases: bump `NESH_VERSION` in `src/core/shell.h` **and** the version strings
in `README.md`, `docs/index.html`, both manuals (including the tested example
output), then tag. The annotated tag message becomes the release notes; CI
builds `nesh.efi` and publishes it. `nesh-usb.img` is uploaded by hand
(`make usb`, then `gh release upload`).

## Where the project stands

Tested in QEMU with OVMF, including Secure Boot with generated test keys.
**On real hardware, only a first smoke test** (September 2026, by the user): on a
Chuwi tablet and a server with a Gigabyte motherboard the prompt came up, `dir`
worked and NESH booted Linux — nothing else was tried, firmware vendor and
Secure Boot state were not recorded. Broader hardware coverage is still the
main open item, and the reason for the announcement on the OSDev forum asking
for reports. Also open:
`find`, `crc32`, `sha256`, `efiinfo`, a `secureboot` key listing,
`bootmgr scan`, `ON ERROR`, `partmgr.efi` (designed, D21); `https` and host
names in `http` are untested; ARM64 and IA32 builds do not exist yet.
