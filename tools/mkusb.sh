#!/bin/sh
# Build a bootable disk image with NESH on it: GPT + one EFI system
# partition (FAT32) holding \EFI\BOOT\BOOTX64.EFI, the examples and a
# README. Write it to a USB stick with
#     dd if=nesh-usb.img of=/dev/sdX bs=4M conv=fsync
# or start it in QEMU with
#     qemu-system-x86_64 -bios OVMF.fd -drive format=raw,file=nesh-usb.img
#
# Needs: mkfs.fat (dosfstools >= 4.2, for --offset), sfdisk, mtools.
# Usage: tools/mkusb.sh build/nesh.efi build/nesh-usb.img
set -eu

EFI=${1:?usage: mkusb.sh nesh.efi image.img}
IMG=${2:?usage: mkusb.sh nesh.efi image.img}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

SIZE_MB=64
PART_START=2048                 # 1 MiB, the usual alignment
MKFS=$(command -v mkfs.fat || echo /sbin/mkfs.fat)
SFDISK=$(command -v sfdisk || echo /sbin/sfdisk)

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none

# GPT with one EFI system partition covering the rest of the image
"$SFDISK" --quiet --label gpt "$IMG" >/dev/null <<EOF
start=$PART_START, type=uefi, name="NESH"
EOF

# the last 33 sectors belong to the backup GPT, so the filesystem stops before
# them; mkfs.fat counts in 1 KiB blocks and warns that the file is larger.
"$MKFS" --offset "$PART_START" -F 32 -n NESH "$IMG" \
        $(( (SIZE_MB * 1024 * 1024 / 512 - PART_START - 33) / 2 )) 2>&1 \
        | grep -v -e "block count mismatch" -e "^mkfs.fat " || true

# everything below writes into the partition, not the whole image
export MTOOLS_SKIP_CHECK=1
M="$IMG@@$((PART_START * 512))"

mmd -i "$M" ::/EFI ::/EFI/BOOT ::/examples
mcopy -i "$M" "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$M" "$EFI" ::/nesh.efi
mcopy -i "$M" "$ROOT"/examples/*.nsb ::/examples/

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
sed 's/$/\r/' > "$TMP" <<'EOF'
NESH - New EFI Shell
====================

This stick starts NESH from the firmware boot menu: it is the default
boot loader of the EFI system partition (\EFI\BOOT\BOOTX64.EFI).

  - With Secure Boot active the firmware refuses it: the file is not
    signed. Sign it with your own key, or turn Secure Boot off.
  - Inside NESH, type "help" for the commands and "help basic" for the
    scripting language.
  - The examples folder has ready-made scripts. Try:
        fs0:\> examples\findefi.nsb
    and read the rest with "more fs0:\examples\bootmenu.nsb".
  - A script named startup.nsb in the root of this volume runs at every
    start; examples\bootmenu.nsb is an interactive boot menu.

You are holding the one thing the project cannot test by itself: a real
machine. NESH has only ever run in QEMU, so please say how it went, whether
it worked or not. These four lines write a report to this stick:

      fs0:\> ver > fs0:\report.txt
      fs0:\> sysinfo >> fs0:\report.txt
      fs0:\> map >> fs0:\report.txt
      fs0:\> smbiosview -t 1 >> fs0:\report.txt

Attach report.txt to a hardware report at
https://github.com/nic-fio/NG-EFI_SHELL/issues/new?template=hardware-report.yml

Documentation: https://nic-fio.github.io/NG-EFI_SHELL/
Project:       https://github.com/nic-fio/NG-EFI_SHELL
EOF
mcopy -i "$M" "$TMP" ::/README.txt

printf '%s: %s MiB, %s files\n' "$IMG" "$SIZE_MB" \
       "$(mdir -i "$M" -b -/ ::/ | grep -c .)"
