#!/bin/sh
# Boots nesh.efi in QEMU with OVMF from a virtual FAT disk (build/esp).
#   run-qemu.sh OVMF.fd nesh.efi           interactive, serial console
#   run-qemu.sh --test OVMF.fd nesh.efi    runs tests/efi/run.nsb, prints the log, exits
set -e
TEST=0
NET=0
if [ "$1" = "--test" ]; then TEST=1; shift; fi
if [ "$1" = "--nettest" ]; then TEST=1; NET=1; shift; fi
OVMF=$1
EFI=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ESP=$ROOT/build/esp
[ -f "$OVMF" ] || { echo "OVMF firmware not found: $OVMF (set OVMF=...)"; exit 1; }

rm -rf "$ESP"
mkdir -p "$ESP/EFI/BOOT"
cp "$EFI" "$ESP/EFI/BOOT/BOOTX64.EFI"
cp "$EFI" "$ESP/nesh.efi"
cp -r "$ROOT/examples" "$ESP/examples" 2>/dev/null || true
# a writable copy of the firmware: -bios keeps variables only in memory
cp "$OVMF" "$ROOT/build/ovmf.fd"

# fs0: the files above (read-only: QEMU's writable vvfat is unreliable);
# fs1: a real FAT image for everything that writes (kept between runs).
WORK=$ROOT/build/work.img
if [ ! -f "$WORK" ] || [ $TEST = 1 ]; then
    rm -f "$WORK"
    /sbin/mkfs.fat -C -n NESHWORK "$WORK" 65536 >/dev/null
fi
DISKS="-drive if=virtio,readonly=on,format=raw,file=fat:$ESP -drive if=virtio,format=raw,file=$WORK"

ACCEL="-accel tcg"
[ -w /dev/kvm ] && ACCEL="-accel kvm -accel tcg"

if [ $NET = 1 ]; then
    # network: QEMU user networking (DHCP, TFTP server on 10.0.2.2; IPv6: router
    # advertisements for fec0::/64, host fec0::2) + an HTTP server on the host
    SRV=$ROOT/build/netsrv
    rm -rf "$SRV"; mkdir -p "$SRV"
    printf 'hello from the host\n' > "$SRV/hello.txt"
    head -c 3000000 /dev/urandom > "$SRV/big.bin"
    cp -r "$ROOT/tests/fixtures/netdrv" "$ESP/netdrv"
    cp "$ROOT/tests/efi/net.nsb" "$ESP/startup.nsb"
    (cd "$SRV" && exec python3 -m http.server --bind :: 18080 >/dev/null 2>&1) & # IPv4 and IPv6
    HTTPPID=$!
    sleep 1
    LOG=$ROOT/build/qemu-nettest.log
    timeout 240 qemu-system-x86_64 $ACCEL -m 512 -bios "$ROOT/build/ovmf.fd" \
        -drive if=none,id=esp,readonly=on,format=raw,file=fat:$ESP -device virtio-blk-pci,drive=esp,bootindex=0 \
        -drive if=virtio,format=raw,file=$WORK \
        -netdev user,id=n0,tftp="$SRV" -device virtio-net-pci,netdev=n0,romfile= -device virtio-rng-pci \
        -display none -serial file:"$LOG" -no-reboot || true
    kill $HTTPPID 2>/dev/null
    sed -e 's/\x1b\[[0-9;=?]*[A-Za-z]//g' -e 's/\r//g' "$LOG" > "$LOG.txt"
    sed -n '/=== NESH TEST START ===/,/=== NESH TEST END ===/p' "$LOG.txt"
    if grep -q "=== NESH TEST END ===" "$LOG.txt" && grep -q "^failures:0" "$LOG.txt" && ! grep -q "^FAIL" "$LOG.txt"; then
        echo "qemu network tests: OK"
    else
        echo "qemu network tests: FAILED (full log: $LOG.txt)"
        exit 1
    fi
elif [ $TEST = 1 ]; then
    cp -r "$ROOT/tests/efi" "$ESP/tests"
    mkdir -p "$ESP/tests/apps"
    cp "$ROOT/build/tests/shelltest.efi" "$ESP/tests/apps/"
    cp "$ROOT"/tests/fixtures/edk2/*.efi "$ESP/tests/apps/"
    # PCI option ROM for the loadpcirom test, from the QEMU installation (iPXE, GPL)
    ROM=${QEMU_ROM:-/usr/share/qemu/efi-virtio.rom}
    [ -f "$ROM" ] && cp "$ROM" "$ESP/tests/apps/efi-virtio.rom"
    cp "$ROOT/tests/efi/startup.nsb" "$ESP/startup.nsb"
    LOG=$ROOT/build/qemu-test.log
    timeout 180 qemu-system-x86_64 $ACCEL -m 512 -bios "$ROOT/build/ovmf.fd" \
        $DISKS -net none -display none \
        -serial file:"$LOG" -no-reboot || true
    # strip terminal escape sequences and carriage returns
    sed -e 's/\x1b\[[0-9;=?]*[A-Za-z]//g' -e 's/\r//g' "$LOG" > "$LOG.txt"
    sed -n '/=== NESH TEST START ===/,/=== NESH TEST END ===/p' "$LOG.txt"
    if grep -q "=== NESH TEST END ===" "$LOG.txt" && grep -q "^failures:0" "$LOG.txt" && ! grep -q "^FAIL" "$LOG.txt"; then
        echo "qemu tests: OK"
    else
        echo "qemu tests: FAILED (full log: $LOG.txt)"
        exit 1
    fi
else
    exec qemu-system-x86_64 $ACCEL -m 512 -bios "$ROOT/build/ovmf.fd" \
        $DISKS -net none -nographic
fi
