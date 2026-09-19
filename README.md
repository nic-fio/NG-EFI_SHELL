# NESH — New EFI Shell

A modern command shell for UEFI firmware, with a real scripting language.
One file, `nesh.efi`: nothing to install, one signature for Secure Boot, no
EDK2 code.

```
fs0:\> map
fs0:\> ls -l \EFI\BOOT
fs0:\> bootmgr add fs0:\EFI\debian\shimx64.efi "Debian" -t
fs0:\> PRINT HEX$(&H1000 * 4)
4000
fs0:\> n = RECORDS(RUN$("map -data"), v$()) : PRINT n; " volumes and disks"
```

## Highlights

- **The UEFI Shell command set, rewritten from scratch**: files, disks, UEFI
  variables, drivers and devices, PCI/SMBIOS/ACPI, memory, network (IPv4 and
  IPv6: `ifconfig`, `ping`, `tftp`, `http`), editors. UEFI Shell options are
  accepted too.
- **NESH BASIC** scripts (`.nsb`): variables, loops, `SUB`/`FUNCTION`, strings,
  arrays, menus, files, and `RUN`/`RUN$` to drive commands.
- **Script-friendly output**: `-data` prints `key=value` records that scripts
  read with `RECORDS` and `FIELD$`.
- **Safe boot management** with `bootmgr`: dry run, confirmation, automatic
  backups, check for missing files. `bcfg` is there too.
- **Runs UEFI Shell applications**: NESH implements `EFI_SHELL_PROTOCOL` 2.2.
- **Friendly prompt**: history with search, Tab completion, aliases, BASIC at
  the prompt.
- **Secure Boot aware**: low-level hardware writes are disabled while Secure
  Boot is active.

## Documentation

| Document | For |
|---|---|
| [User manual](docs/user-manual.html) | Using NESH and writing scripts: every command and function, with examples. |
| [Developer manual](docs/developer-manual.html) | Architecture, build, internals, testing, how to extend NESH. |
| [Design decisions and history](docs/decisions-and-history.md) | Why NESH is the way it is. |

The manuals are HTML pages that work offline: open them in a browser from a
clone of the repository. Inside NESH, `help` and `help NAME` show the same
command reference.

## Build and test

Requirements: `gcc`, GNU `ld`, `make`, `python3`; for the UEFI tests
`qemu-system-x86_64`, OVMF firmware and `mkfs.fat`.

```
make                 # build/nesh.efi (UEFI x86-64) and build/nesh-host (Linux test build)
make test            # language and command tests, documentation checks
make qemu-test       # automated tests inside QEMU/OVMF
make qemu-nettest    # network tests (IPv4 and IPv6) inside QEMU
make qemu            # interactive session in QEMU (serial console)
```

`OVMF=/path/to/OVMF.fd` selects the firmware image.

## Install

Copy `build/nesh.efi` to a FAT-formatted USB stick or EFI system partition,
then either copy it as `\EFI\BOOT\BOOTX64.EFI` to boot it directly from the
firmware boot menu, start it from another shell, or add a boot entry with
`bootmgr add`. With Secure Boot active, sign it with a trusted key first
(see the user manual).

## Status

Version 0.1.0. Tested in QEMU with OVMF; not yet tested on real hardware or
with Secure Boot enabled. See the open questions at the end of the
[decisions document](docs/decisions-and-history.md).
