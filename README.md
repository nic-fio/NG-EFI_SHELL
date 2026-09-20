# NESH — New EFI Shell

[![CI](https://github.com/nic-fio/NG-EFI_SHELL/actions/workflows/ci.yml/badge.svg)](https://github.com/nic-fio/NG-EFI_SHELL/actions/workflows/ci.yml)
[Documentation](https://nic-fio.github.io/NG-EFI_SHELL/) · [Download](https://github.com/nic-fio/NG-EFI_SHELL/releases/latest)

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
| [User manual](https://nic-fio.github.io/NG-EFI_SHELL/user-manual.html) | Using NESH and writing scripts: every command and function, with examples. |
| [Developer manual](https://nic-fio.github.io/NG-EFI_SHELL/developer-manual.html) | Architecture, build, internals, testing, how to extend NESH. |
| [Design decisions and history](docs/decisions-and-history.md) | Why NESH is the way it is. |

The manuals are HTML pages, so GitHub shows them as source code: read them
online at **https://nic-fio.github.io/NG-EFI_SHELL/**, or open
`docs/user-manual.html` in a browser from a clone (they work offline). Inside
NESH, `help` and `help NAME` show the same command reference.

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

## License

Copyright (c) 2026 nic-fio. NESH is released under the **Creative Commons
Attribution-NonCommercial 4.0 International** license
([CC BY-NC 4.0](LICENSE)): free to use, share and modify for non-commercial
purposes; a modified version you share must credit the original project and
say that it was changed. Commercial use needs permission.

Note that a non-commercial license is not "open source" by the OSI definition.
Third-party components keep their own licenses, listed in [NOTICE.md](NOTICE.md).

## Status

Version 0.2.0. Tested in QEMU with OVMF, including Secure Boot with test keys;
not yet tested on real hardware. See the open questions at the end of the
[decisions document](docs/decisions-and-history.md).
