# Test fixtures

Third-party binaries used **only by the tests** (they are not part of NESH and
are not included in `nesh.efi`).

| Path | What | Origin | License |
|---|---|---|---|
| `edk2/*.efi` | UEFI Shell applications (`edit`, `pci`, `ping`, `tftp`, `usb`) | Built from EDK2 ShellPkg | BSD-2-Clause-Patent, see `EDK2-License.txt` |
| `netdrv/*.efi` | Firmware network drivers (MNP, ARP, IP4/6, UDP4/6, DHCP4/6, MTFTP4/6, TCP, DNS, HTTP) and `Hash2DxeCrypto.efi` | Extracted from the OVMF firmware image of Debian (EDK2) | BSD-2-Clause-Patent, see `EDK2-License.txt`; `Hash2DxeCrypto.efi` also contains OpenSSL code (Apache-2.0) |

They let the QEMU tests check that applications written for the UEFI Shell run
under NESH, and load the network stack that Debian's OVMF only loads for
network boot.

The PCI option ROM used by the `loadpcirom` test (`efi-virtio.rom`, iPXE,
GPL-2.0) is not stored here: `tools/run-qemu.sh` copies it from the QEMU
installation (`QEMU_ROM=`, default `/usr/share/qemu/efi-virtio.rom`, package
`ipxe-qemu` on Debian). Without it that test is skipped.
