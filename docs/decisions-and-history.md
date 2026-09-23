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
- **Verified** with Secure Boot really enabled (`make qemu-sbtest`, own test
  keys enrolled in OVMF): the unsigned image is refused by the firmware, the
  signed one runs with hardware reads working and low-level writes refused, and
  unsigned applications and drivers are refused. The test also showed that OVMF
  answers `EFI_ACCESS_DENIED` rather than `EFI_SECURITY_VIOLATION`, so NESH
  printed a bare "access denied"; it now recognizes both
  (`efi_blocked_by_secure_boot`) and says why the image was refused.

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

### D16b. Screen resolution and text size

- **Context.** `gop` could only select a graphics mode by number, and the screen
  resolution did not stay: after `SetMode` the command called
  `ConOut->Reset()` "to let the text console adapt", and the firmware console
  driver put its own resolution back.
- **Decision.** `gop` selects a mode by resolution (`gop 1024 768`), by number,
  `-max` or `-fit`; the reset is gone. Because the firmware draws text in fixed
  cells (8 x 19 pixels) and decides its text sizes when its console driver
  starts, a large screen shows the text in a corner: `mode -max` takes the
  largest text size available and `gop -fit` lowers the resolution to the
  smallest one that still holds the text console, so the text fills the screen.
- **Safety.** Before switching to a smaller resolution NESH moves the console to
  a text mode that fits, so the firmware never draws outside the frame buffer.
- **`mode -fill`.** Added after the owner asked how to make the text use a whole
  1680x1050 screen. Verified by photographing the emulated screen (QEMU monitor
  `screendump`): at 1680x1050 the text was a 100x31 block in the middle; after
  `reconnect -r` the firmware rebuilt its list and offered 160x42, which fills
  the screen. `mode -fill` does those two steps. It cannot keep a resolution
  chosen with `gop`, because restarting the drivers also resets the video
  driver: that combination has to come from the firmware setup.
- **Not in the automatic tests**: `reconnect -r` disconnects the serial console
  the QEMU tests read, so `mode -fill` is checked by hand with a screendump.

### D16c. Paging of long output

- **Context.** A UEFI console cannot be scrolled back, so the beginning of a long
  output (`help`, `dh`, `drivers`) was lost. The UEFI Shell option `-b` was
  accepted and ignored, and there was no `more`.
- **Decision.** Paging lives in the console layer (`out_paging` in `con.c`), so
  every command gets it without changes: output to the console stops at every
  screenful with `-- More --` (Space: a page, Enter: a line, q: stop).
- **When.** Automatically for commands typed at the prompt whose output goes to
  the screen; never inside scripts or when the output is redirected or captured,
  so automation is unaffected. `-b` turns it on for one command anywhere (UEFI
  Shell compatibility), `set pager off` turns the automatic paging off, and the
  new `more` command always pages.
- **Consequences.** `q` also stops the command (through the same flag as
  Ctrl-C, but without reporting an interruption). `-b` is removed from the
  arguments before the command sees it, except for commands that use it
  themselves (`exit -b`, and `echo`, which prints its words as they are):
  they carry the flag `CMD_ARG_B`.

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

### D20. License: Apache 2.0 with the Commons Clause

- **Context.** The repository was published with a provisional "all rights
  reserved" notice. The owner then asked for a real license.
- **First requirements.** Free use, free redistribution, modification for one's
  own use, credit when a modified version is shared, "no commercial use".
  That maps onto CC BY-NC 4.0, which was adopted first.
- **What the owner actually wants.** "l'azienda di turno che sfrutta il lavoro
  altrui per rivendere il progetto. Se vogliono acquistino il progetto" — the
  target is **resale**, not use. CC BY-NC was too wide: it also forbids a
  technician using NESH while repairing a computer for money, or a company
  using it on its own machines. Asked about that boundary, the owner confirmed
  that such use must stay free.
- **Options.** (a) Keep CC BY-NC 4.0 (bans every commercial use);
  (b) Business Source License 1.1, made for this, but its **Change Date** is
  mandatory and at most four years, after which the work becomes open source —
  which would remove exactly the value the owner wants to be able to sell;
  (c) a permissive license plus the **Commons Clause**, a condition that removes
  the right to *sell* the software or services substantially based on it, with
  no expiry.
- **Decision.** (c): Apache License 2.0 with the Commons Clause v1.0. Apache 2.0
  already requires keeping the notices and stating changes (the attribution the
  owner asked for) and adds a patent grant; the Commons Clause draws the line at
  selling. A commercial license, without the condition, is asked for by opening
  an issue in the repository.
- **Consequences.** The project is *not* open source by the OSI definition (the
  Commons Clause is a restriction on the Apache grant), so some distributions
  and catalogues will not accept it. The wording "value derives, entirely or
  substantially, from the functionality of the Software" is the usual
  Commons Clause text: it is the boundary the owner asked for, but it is
  interpreted case by case. Third-party components keep their own licenses
  (`NOTICE.md`).

### D21. Partition management: a separate `partmgr.efi`, not a NESH command

*Status: designed, not implemented.*

- **Context.** The owner proposed adding "un paio di funzioni utili, come un
  partition manager". The EDK2 shell has nothing for it; people who prepare
  service sticks (the Win-Raid audience) usually boot a Linux live system to
  do it.
- **First design, inside NESH.** A `partmgr` command with subcommands (`list`,
  `delete`, `create`, `backup`, `restore`) was designed step by step. The
  compact form `partmgr create blk2 gpt esp:512M linux:rest` was rejected as
  "decisamente criptico"; a `parted`-like form with one `add` per partition
  and options such as `-type primary|extended` followed, and with it MBR
  extended partitions. The design kept growing beyond "a couple of useful
  functions", and the owner stopped it: "Niente gestione delle partizioni da
  nesh".
- **Decision.** Partition management is a **separate application,
  `partmgr.efi`, with a full-screen interface**, "una utility, non una
  funzione di nesh". It lives **in this repository** and ships in the same
  release as `nesh.efi` — owner: "alla fine nesh e' pur sempre una applet
  .efi". It reuses the platform layer, the screen code of `edit`/`hexedit` and
  the MBR/GPT decoding of `dblk`; the Linux build can work on disk image files,
  so it can be tested without real disks.
- **D5 is unchanged.** `nesh.efi` stays one file with everything built in and
  no partition management. `partmgr.efi` is a second product, started like any
  EFI application (from NESH, another shell or the firmware boot menu), and is
  signed separately for Secure Boot.
- **Words.** To *delete* a partition removes it from the table; to *wipe* it
  destroys its contents. In the owner's Italian: *eliminare* and *cancellare*.
  "Zap", the first name proposed for wiping, was dropped: in `gdisk`/`sgdisk`
  it means destroying the partition *table*, the opposite meaning.
- **Functions.** Show disks and partitions; delete the whole table; create an
  empty GPT or MBR table; add a partition in any free space; delete one
  partition; change a partition's type and name; set the MBR active flag;
  back up the partition table to a file and restore it; wipe a partition.
  Out of scope: resizing, moving, formatting, and any change to data inside a
  partition other than wiping.
- **GPT and MBR, MBR complete.** Both formats are read and created. Owner, on
  creating MBR tables without extended partitions: "sarebbe una
  contraddizione". So MBR includes extended and logical partitions (adding on
  an MBR disk asks Primary or Logical and creates the extended partition when
  needed), the active flag and the CHS fields.
- **A new table starts from zero.** Creating a table is destructive: the whole
  MBR sector is rebuilt and its 440-byte boot code area is zeroed, the GPT
  protective MBR included, and the disk gets new identifiers (MBR signature,
  GPT disk GUID). Owner: "la gestione dell'mbr e' roba da bootloader, che non
  riguarda partmgr". The alternative of keeping the old boot code, or writing
  an own MBR boot program, was rejected.
- **Changes are written all together.** As in `cfdisk` and `gdisk`, the screen
  shows the table as it will be and marks unwritten changes; nothing reaches
  the disk until **Write**. Quitting without Write leaves the disk untouched.
- **Safety.** Before every destructive operation a very readable warning, then
  the disk name must be typed to confirm. **No dry run**: the confirmation
  already shows what will be written. **No automatic backup**: owner, "se uno
  vuole fa' prima il backup"; Backup is an explicit function. The disk the
  program was started from is shown but always read-only, with no override.
  Writes are allowed with Secure Boot active: writing a partition table is a
  disk write, like `hexedit` on disk blocks, and does not get around the
  protection (D6 unchanged).
- **Wipe.** Two passes over the whole partition: random data, then zeros (owner's
  choice). It cannot be staged, so it runs at once, with its own warning and
  confirmation, a progress bar, and Esc to stop. The manual must say that on
  SSD, NVMe and USB flash overwriting does not guarantee that the old data is
  gone (wear levelling, reserve cells); the drives' own ATA Secure Erase and
  NVMe Sanitize work only on whole disks and are left out.
- **Interface.** Screen 1 lists the disks (model, size, table type, number of
  partitions). Screen 2 shows the partitions and the free space in disk order,
  with a key bar at the bottom (New, Delete, Type, Rename, Active, Wipe,
  Backup, Restore, New table, Delete table, Write, Back). Types are chosen from
  a list, never typed as codes; the Microsoft Reserved Partition is called
  `msreserved`, not `msr`, to avoid confusion with D13. A new partition is
  given by **start and size** (owner's choice over start and end): the start
  defaults to the beginning of the selected free space, the size to all of it,
  with 1 MiB alignment and sizes such as `512M` or `20G`.
- **Professional and friendly.** Owner: "voglio che l'app abbia
  un'interfaccia professionale ma user-friendly". Every operation that takes
  time shows its progress; for the wipe a **progress bar is mandatory**, with
  the pass (1 of 2 random data, 2 of 2 zeros), the percentage, the amount
  written, the speed and the estimated time left. Esc asks before stopping.
- **No command line.** `partmgr.efi` has only the full-screen interface, so it
  cannot be driven by scripts; a proposal for `list`/`backup`/`restore` on the
  command line was declined.

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
| **8. Screen** | Screen-resolution options added on the owner's suggestion (`gop WIDTH HEIGHT`, `-max`, `-fit`): it turned out that changing the resolution had no lasting effect at all, and that the text console covers only part of a large screen (a firmware property, now explained in the manual). |
| **9. Console** | Paging of long output (D16c), after the owner noticed that a long `help` scrolls away and cannot be read back. |
| **10. Secure Boot** | Tested for real in QEMU with own test keys enrolled in OVMF (`make qemu-sbtest`, part of CI): unsigned images refused by the firmware, hardware writes refused, clock/serial allowed, keys unchangeable. One defect found and fixed: refusals were reported as a bare "access denied". |
| **11. Publication** | Repository published on GitHub as `NG-EFI_SHELL`, public (owner's request); provisional all-rights-reserved license; manuals online with GitHub Pages; GitHub Actions builds and tests every push and publishes a Release with `nesh.efi` for each version tag (binaries are distributed as Releases, not committed to the repository). |

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
| Real hardware | The automatic tests run in QEMU/OVMF. NESH has been started on two real machines, a Chuwi tablet and a server with a Gigabyte motherboard: the prompt came up, `dir` listed the files, and NESH booted Linux. Nothing else was tried there, and the firmware vendor and Secure Boot state were not recorded; the other commands, and firmware from AMI, Insyde or Phoenix in general, remain unverified. |
| Architectures | x86-64 only; AArch64 and IA32 builds are possible future work. |
| `https` | Needs a TLS driver in the firmware; untested. Host names in `http` URLs (DNS) are untested. |
| UEFI Shell options not supported | `time -tz/-d`; `-l LANG` of `devices`/`devtree` is accepted and ignored. |
| Not implemented | See "The original plan" above. |
| `partmgr.efi` | Designed (D21), not implemented yet. |
| License | Settled: Apache 2.0 with the Commons Clause (see D20). Not "open source" by the OSI definition, so some distributions and catalogues will not accept the project. Commercial licences are granted on request. |
