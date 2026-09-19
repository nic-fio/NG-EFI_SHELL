# NESH — Design decisions and project history

This document records **why** NESH is the way it is: the decisions taken while
designing and building it, the alternatives that were considered, and the
history of the project. It complements the
[user manual](user-manual.html) (what NESH does) and the
[developer manual](developer-manual.html) (how it is built).

Decisions were discussed with the project owner one at a time, in Italian;
where a decision came from the owner, the original words are quoted.

**Format of each decision:** context → options → decision → consequences.
Status is *accepted* unless noted otherwise.

---

## Contents

1. [Goals](#1-goals)
2. [Decisions](#2-decisions)
3. [History](#3-history)
4. [Open questions and known gaps](#4-open-questions-and-known-gaps)

---

## 1. Goals

The project started with one sentence from the owner:

> "lo scopo di questa sessione e' produrre una shell efi di nuova generazione
> molto piu' utile e flessibile di quella attualmente disponibile, rendendola
> adatta anche per uso di script"
>
> *(the goal is a new-generation EFI shell, much more useful and flexible than
> the one available today, and suitable for scripting)*

From it came the working rules of the project:

- **Agree on features before writing code** ("Prima di cominciare a scrivere,
  meglio decidere quali funzioni potrebbe essere utile implementare").
- **Discuss decisions one at a time, in simple language** ("Sulle decisioni da
  prendere discutiamo punto per punto, usando un linguaggio piu' semplice").
- **Documentation has the same importance as code** ("un progetto serio da' alla
  documentazione la stessa importanza del codice").

---

## 2. Decisions

### D1. A BASIC-like scripting language

- **Context.** The UEFI Shell's `.nsh` scripts are batch files: `if`, `for`,
  `goto`, `%1`, no expressions, no functions, no string handling.
- **Options.** (a) a Unix-shell language like bash; (b) extend `.nsh`;
  (c) embed an existing language (Lua, MicroPython); (d) a small BASIC.
- **Decision.** (d), proposed by the owner: "Io proporrei di usare un linguaggio
  di script non eccessivamente complicato: variabili, cicli, qualche operazione
  sulle stringhe e qualche conversione, farei qualcosa di piu' simile ad un
  basic che a bash".
- **Consequences.** NESH BASIC: structured (no line numbers), case-insensitive,
  64-bit integers and UTF-8 strings (`$` suffix), one-dimensional arrays,
  `SUB`/`FUNCTION`/`LOCAL`, `SELECT CASE`, `GOTO` to labels. Scripts use the
  extension `.nsb`. `.nsh` scripts are not supported.

### D2. Commands are run with `RUN`

- **Context.** In a BASIC, a bare word is a variable or a procedure; mixing shell
  command syntax into the language makes both ambiguous.
- **Decision.** Proposed by the owner ("perche' non fare in modo di lanciare i
  comandi con un comando RUN "comando"?"): scripts run commands with
  `RUN "cmd args"` or `RUN "cmd", arg1$, arg2$` (arguments passed as they are,
  safe with spaces), capture output with `RUN$()` and read the exit code in
  `ERR`. `RUN … TO/APPEND file` redirects.
- **Consequences.** At the **interactive prompt only**, a line that is not a
  BASIC statement is run as a command (an implicit `RUN`), so `ls fs0:` works as
  in any shell. In scripts `RUN` is mandatory.

### D3. EFI programs are simply invoked

- **Decision.** Owner: "per l'avvio dei files .efi io mi limiterei semplicemente
  ad invocarli, o a eseguirli col comando RUN nel caso di uno script". `.efi`
  files and `.nsb` scripts run by typing their name (extension optional,
  searched along `path`). `bootmgr` manages boot entries but does not start
  programs.

### D4. Boot loader management is part of the shell

- **Decision.** Owner: "poiche' parliamo di EFI, potrebbe essere utile anche la
  possibilita' di gestire i bootloader?". NESH has `bootmgr` (its own syntax) in
  addition to the UEFI Shell's `bcfg`.
- **Consequences.** Every change goes through a *plan*: shown, optionally dry
  run (`-n`), confirmed (`-y` skips), preceded by an automatic backup of all boot
  variables (`-B` skips), then applied. `bootmgr next` does not ask for
  confirmation because it affects a single boot. BASIC functions `BOOT*` and
  `bootmgr -data` give scripts access to the entries.

### D5. One single `.efi` file

- **Decision.** Owner: "meglio se questo progetto risulti come un unico file
  .efi: questo rende anche piu' semplice firmarlo nel caso di sistemi con secure
  boot attivo".
- **Consequences.** All commands, help texts and functions are built in; nothing
  is loaded at run time. Files on disk (history, `startup.nsb`, backups) are
  optional data.

### D6. Secure Boot: hardware is read-only

- **Decision.** Owner: "nel caso di secure boot attivo lasciamo pure che i
  valori hardware possano essere solo letti e non scritti".
- **Scope.** Low-level writes that could be used to get around the protection:
  memory, MMIO, I/O ports and PCI registers (`mm` writes, `hexedit -m` saves).
  They go through one function, `hw_write_allowed()`. The firmware itself keeps
  enforcing signatures on `LoadImage`, which NESH never bypasses.
- **Note.** While writing the manual the guard was briefly extended to
  `sermode` and `timezone`; it was restored to the scope above because `date`
  and `time` (same firmware service as `timezone`) were not covered and the
  extension had not been agreed. The owner then confirmed the scope: clock and
  serial port settings stay allowed ("orologio e porta seriale non dovrebbero
  rappresentare un pericolo per la sicurezza").

### D7. No EDK2 code, own toolchain

- **Context.** UEFI applications are usually built with EDK2 or gnu-efi.
- **Decision.** Neither is used. NESH has its own UEFI headers, freestanding C
  library and `printf`, and builds with the ordinary Linux GCC and ld. The ELF
  shared object is converted to PE32+ by `tools/elf2efi.py`.
- **Why the converter.** `objcopy --target=efi-app-x86_64` was tried first and
  produced images with corrupted relocations and missing sections. The converter
  accepts only `R_X86_64_RELATIVE` relocations and fails loudly otherwise.
- **Also.** The interpreter is recursive and UEFI guarantees only 128 KiB of
  stack, so `efi_main` switches to a private 1 MiB stack.

### D8. Portable core and a Linux build for tests

- **Decision.** The shell and the interpreter only use a small platform layer
  (`pal.h`) with a UEFI and a Linux implementation. The Linux build, with
  AddressSanitizer and UBSan, runs the language and command tests in seconds;
  UEFI parts are tested in QEMU with OVMF firmware.

### D9. UEFI Shell applications must work

- **Context.** Early versions could start `.efi` programs, but applications built
  with the EDK2 ShellLib crashed: they need `EFI_SHELL_PROTOCOL`.
- **Decision.** Owner: "le app .efi non partono. Devono funzionare". NESH
  implements EFI_SHELL_PROTOCOL 2.2 and EFI_SHELL_PARAMETERS_PROTOCOL itself.
- **Consequences.** The EDK2 builds of `pci`, `usb`, `tftp`, `ping` and `edit`
  run under NESH; `tests/apps/shelltest.c` checks every protocol function. When
  NESH is started from another shell it replaces that shell's protocol and
  restores it on exit.

### D10. Rewrite the UEFI Shell commands, accept their options

- **Decision.** Owner: "sganciamoci da EDK2. I comandi che la nostra shell non
  dispone ma ED2K li riscrivi". Every UEFI Shell command was rewritten from
  scratch, and commands accept the UEFI Shell options as well as NESH's own
  (options that make no sense in NESH, such as `-b` paging, are accepted and
  ignored).
- **Work order** agreed with the owner: drivers and devices; `dmpstore`,
  `setvar`, `bcfg`; disks and files; memory and hardware; then `alias`,
  `parse`, `stall`, compression; the editors; the network.
- **Compatibility checks.** `dmpstore` files are exchanged in both directions
  with the EDK2 shell; the UEFI compressor's output is decompressed by the
  firmware.

### D11. Environment variables and aliases, separate from BASIC

- **Decision.** Owner: "implementa la piena funzionalita' delle variabili di
  ambiente e ok per la separazione delle variabili degli script da quelle di
  ambiente".
- **Consequences.** `set`, `ENV$`, `SETENV`, `DELENV`. Permanent variables and
  aliases are stored as UEFI variables with the same GUIDs as the UEFI Shell, so
  both shells share them. `path` is used to find commands.

### D12. No legacy EFI 1.10 shell interface

- **Decision.** Owner: "lasciamo riposare in pace il vecchiume defunto". Programs
  that only use the pre-2009 EFI shell interface are not supported.

### D13. No `msr` command

- **Decision.** Owner: "msr fuori". Reading a model-specific register that does
  not exist raises a general-protection fault that hangs the machine; the risk
  outweighs the use.

### D14. `-data` instead of `-sfo`

- **Context.** UEFI Shell commands offer `-sfo`, a comma-separated "standard
  format output" that is awkward to parse in scripts.
- **Options.** (1) implement `-sfo` as is; (2) implement `-sfo` plus a parser
  function; (3) a new format designed for NESH BASIC.
- **Decision.** Option 3. Owner: "Io direi la 3. Sfruttiamo le possibilita' di un
  progetto tutto nuovo."
- **Consequences.** `command -data` prints records of `key=value` lines separated
  by an empty line (booleans `yes`/`no`, sizes decimal, handles and IDs hex).
  `RECORDS(text$, arr$())` and `FIELD$(record$, key$)` read them. Supported by
  `ls dir stat map vol ver sysinfo memmap date time set alias var bootmgr
  drivers devices dh pci smbiosview acpiview ifconfig ifconfig6`. Commands
  without support reject `-data`; `-sfo` is rejected with a hint.

### D15. IPv6 through the same commands

- **Decision.** Besides `ifconfig6` and `ping6` (UEFI Shell names), `ping`,
  `tftp` and `http` recognize IPv6 addresses by themselves (in URLs, in
  brackets). The source address is chosen to match the destination
  (link-local for `fe80::`).

### D16. Language details decided during implementation

- Labels (`name:`) must be alone on their line, so that `mysub: mysub` stays two
  statements.
- Names of built-in functions cannot be used for variables, arrays, parameters or
  procedures (a parameter called `dir$` would hide `DIR$`).
- Integers only, no floating point: firmware work rarely needs fractions and the
  build avoids FPU state (`-mgeneral-regs-only`).
- `PRINT` adds no spaces around numbers (unlike classic BASIC).
- Truth is `-1` and `AND`/`OR`/`NOT` are bitwise, as in classic BASIC; no
  short-circuit evaluation.
- `SETVAR` writes hex data, `SETVARSTR` a UCS-2 string, `VARNUM` reads a
  little-endian number.
- `exit` from a script stops every running script and leaves the shell;
  `exit /b` (UEFI Shell form) ends only the current script.
- `nesh -k SCRIPT` checks the syntax without running (added for the
  documentation tests, useful to users too).

### D17. English for the product, Italian for the discussion

- Messages, help texts, code comments and the manuals are in English (the
  project is meant to be published). Owner, about the manuals: "va' bene
  l'inglese, se un utente trova difficolta' puo' sempre ricorrere ai traduttori
  online o con LLM".

### D18. Documentation

- **Decision.** Three documents: a user manual and a developer manual in HTML
  ("ricco di grafici, tabelle, grafici mermaid ecc dallo stile professionale,
  strutturato con tabella dei contenuti, indici e capitoli"), and this record of
  decisions in Markdown. The user manual includes short script examples and a
  worked interactive boot menu (owner's requests). Light theme only (owner: "non
  usare temi scuri per i documenti").
- **How it stays correct.** The command reference is generated from the help
  texts in the source (`tools/gen-docs.py`), so `help` and the manual never
  disagree; `make test` fails if a command or BASIC function is undocumented, if
  a help line is longer than 78 columns, or if a BASIC example of the manual no
  longer produces the output printed next to it (`tools/check-doc-examples.py`).
  Mermaid is bundled so the manuals work offline.

### D19. Testing strategy

- Host tests (`tests/host/*.nsb` with expected outputs) for the language and
  portable commands; QEMU/OVMF tests (`tests/efi/run.nsb`, `net.nsb`) for
  everything that needs firmware.
- A QEMU run passes only if the log contains the end marker, `failures:0` and no
  `FAIL` line — before this rule, a script that stopped on an error could be
  reported as passing.
- Every bug fixed gets a test.

---

## 3. History

All work took place in one long working session (2026-09-19), in phases.

| Phase | What happened |
|---|---|
| **1. Design** | Goals and working rules; choice of a BASIC-like language (D1) and `RUN` (D2); boot management (D4); single `.efi` (D5); Secure Boot rule (D6); the full command list with priorities P1–P3. |
| **2. Core (P1)** | Toolchain and `elf2efi.py` (D7); PAL with UEFI and Linux builds (D8); the interpreter; line editor with persistent history, Ctrl-R and Tab completion; file, text and variable commands; `bootmgr`; `memmap`, `sysinfo`, `reset`; running `.efi` files with output capture; `MENU`; `startup.nsb`. Bugs found here: label parsing (fixed with the "label alone" rule), builtin-name shadowing (D16), a hang of the shell under piped input. |
| **3. UEFI Shell compatibility** | EDK2 ShellLib applications did not start: EFI_SHELL_PROTOCOL 2.2 implemented (D9); environment variables and aliases shared with the UEFI Shell (D11); decision to rewrite the UEFI Shell commands (D10); legacy EFI 1.10 interface rejected (D12). |
| **4. UEFI Shell commands** | Seven groups rewritten and tested in QEMU: drivers/devices; `dmpstore`/`setvar`/`bcfg` (files interoperable with EDK2 in both directions); disks and files; memory and hardware (`msr` excluded, D13); utilities with an own UEFI compressor verified against the firmware decompressor; `edit`/`hexedit`; network (IPv4 on the firmware stack; OVMF's network drivers loaded from files in the tests). |
| **5. Script-friendly output** | `-sfo` replaced by `-data` with `RECORDS`/`FIELD$` (D14). The QEMU test runner was found to report "OK" for scripts that stopped on an error; the pass rule was tightened (D19). |
| **6. IPv6** | `ifconfig6`, `ping6`, IPv6 in `ping`/`tftp`/`http` (D15), tested with QEMU user networking; an IPv6 address parser/formatter checked against 29 cases; `ifconfig` now shows the DHCP gateway. |
| **7. Documentation** | Help texts written for all 84 commands by reading their code. That review found and fixed real defects: `mv` could lose the target file if the rename failed; `exit` did not stop a running script; `which` ignored `path`; `load` did not connect drivers when one file failed; `mkdir -p` accepted a file in the path; `vol fs1` without colon showed the wrong volume; missing UEFI Shell options (`reset -c/-fwui`, `pause -q`, `exit /b`, `memmap -b`, `sermode` stop bits 0); the editor lost tab characters and the UTF-8 BOM; the example boot menu failed on read-only volumes and listed hidden entries. User and developer manuals written; examples executed by the test suite. |
| **8. Publication** | Repository published on GitHub as `NG-EFI_SHELL`, public (owner's request); provisional all-rights-reserved license; manuals online with GitHub Pages; GitHub Actions builds and tests every push and publishes a Release with `nesh.efi` for each version tag (binaries are distributed as Releases, not committed to the repository). |

### The original plan and what came of it

Most of the original P1–P3 command list was implemented, often under the UEFI
Shell name (`mem` → `dmem`/`mm`, `acpi` → `acpiview`, `smbios` →
`smbiosview`). Not implemented yet: `more` (paged output), `find`, `crc32`,
`sha256`, `efiinfo` (analysis of an EFI executable), `secureboot` (key
listing), `bootmgr scan`, and the language's `ON ERROR`.

---

## 4. Open questions and known gaps

| Topic | Status |
|---|---|
| Secure Boot | The read-only rule (D6) is implemented but NESH has not yet been run with Secure Boot actually enabled (signing and enrolling a test key in OVMF). |
| Real hardware | All tests run in QEMU/OVMF. Behavior on real firmware (AMI, Insyde, Phoenix) is untested. |
| Architectures | x86-64 only; AArch64 and IA32 builds are possible future work. |
| `https` | Needs a TLS driver in the firmware; untested. Host names in `http` URLs (DNS) are untested. |
| UEFI Shell options not supported | `time -tz/-d`; `-l LANG` of `devices`/`devtree` is accepted and ignored. |
| Not implemented | See "The original plan" above. |
| License | Provisional: all rights reserved (owner: "Per il tipo di licenza non ho ancora deciso nulla di definitivo. Al momento usiamo quella restrittiva, poi la cambieremo"). MIT or BSD-2-Clause were proposed at the start. |
