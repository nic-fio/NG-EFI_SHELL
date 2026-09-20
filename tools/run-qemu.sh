#!/bin/sh
# Boots nesh.efi in QEMU with OVMF from a virtual FAT disk (build/esp).
#   run-qemu.sh OVMF.fd nesh.efi           interactive, serial console
#   run-qemu.sh --test OVMF.fd nesh.efi    runs tests/efi/run.nsb, prints the log, exits
#   run-qemu.sh --sbtest OVMF.fd nesh.efi  Secure Boot tests: own test keys, signed
#                                          image, OVMF_CODE_4M.secboot.fd
set -e
TEST=0
NET=0
if [ "$1" = "--test" ]; then TEST=1; shift; fi
if [ "$1" = "--nettest" ]; then TEST=1; NET=1; shift; fi
SB=0
if [ "$1" = "--sbtest" ]; then SB=1; shift; fi
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

if [ $SB = 1 ]; then
    # Secure Boot: test keys (PK/KEK/db), a signed nesh.efi and the OVMF build
    # with Secure Boot support. The firmware must refuse the unsigned image.
    SBDIR=$ROOT/build/secureboot
    CODE=${OVMF_SECBOOT:-/usr/share/OVMF/OVMF_CODE_4M.secboot.fd}
    VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
    for t in sbsign virt-fw-vars openssl; do
        command -v $t >/dev/null || { echo "secure boot tests skipped: $t not installed"; exit 0; }
    done
    [ -f "$CODE" ] && [ -f "$VARS" ] || { echo "secure boot tests skipped: $CODE not found"; exit 0; }
    mkdir -p "$SBDIR"
    if [ ! -f "$SBDIR/db.key" ]; then
        for k in PK KEK db; do
            openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
                -subj "/CN=NESH test $k/" -keyout "$SBDIR/$k.key" -out "$SBDIR/$k.crt" 2>/dev/null
        done
    fi
    uuid() { python3 -c "import uuid; print(uuid.uuid4())"; }
    cp "$VARS" "$SBDIR/vars.fd"
    virt-fw-vars --input "$SBDIR/vars.fd" --output "$SBDIR/vars.fd" \
        --set-pk "$(uuid)" "$SBDIR/PK.crt" --add-kek "$(uuid)" "$SBDIR/KEK.crt" \
        --add-db "$(uuid)" "$SBDIR/db.crt" --sb >/dev/null
    rm -rf "$ESP"
    mkdir -p "$ESP/EFI/BOOT" "$ESP/apps"
    sbsign --key "$SBDIR/db.key" --cert "$SBDIR/db.crt" --output "$ESP/EFI/BOOT/BOOTX64.EFI" "$EFI" >/dev/null
    cp "$EFI" "$ESP/apps/nesh-unsigned.efi"                    # an unsigned application
    cp "$ROOT"/tests/fixtures/edk2/pci.efi "$ESP/apps/"        # an unsigned UEFI Shell app
    cp "$ROOT"/tests/fixtures/netdrv/Ip4Dxe.efi "$ESP/apps/"   # an unsigned driver
    cp "$ROOT/tests/efi/secureboot.nsb" "$ESP/startup.nsb"
    ACCEL="-accel tcg"
    [ -w /dev/kvm ] && ACCEL="-accel kvm -accel tcg"
    LOG=$ROOT/build/qemu-sbtest.log
    run_sb() { # $1 seconds, $2 log
        cp "$SBDIR/vars.fd" "$SBDIR/vars-run.fd"
        timeout "$1" qemu-system-x86_64 $ACCEL -m 512 -machine q35,smm=on \
            -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
            -drive if=pflash,format=raw,unit=1,file="$SBDIR/vars-run.fd" \
            -drive if=virtio,readonly=on,format=raw,file=fat:$ESP \
            -net none -display none -serial file:"$2" -no-reboot 2>/dev/null || true
        sed -e 's/\x1b\[[0-9;=?]*[A-Za-z]//g' -e 's/\r//g' "$2" > "$2.txt"
    }
    run_sb 180 "$LOG"   # the signed shell runs the tests and shuts down
    sed -n '/=== NESH TEST START ===/,/=== NESH TEST END ===/p' "$LOG.txt"
    ok=1
    grep -q "^failures:0" "$LOG.txt" && grep -q "=== NESH TEST END ===" "$LOG.txt" || ok=0
    # the same image without a signature must not start at all
    cp "$ROOT/build/nesh.efi" "$ESP/EFI/BOOT/BOOTX64.EFI"
    run_sb 45 "$ROOT/build/qemu-sbtest-unsigned.log"   # refused: the firmware waits in its menu
    if grep -qE "Access Denied|Security Violation|failed to load" "$ROOT/build/qemu-sbtest-unsigned.log.txt" &&
       ! grep -q "New EFI Shell" "$ROOT/build/qemu-sbtest-unsigned.log.txt"; then
        echo "PASS unsigned nesh.efi refused by the firmware"
    else
        echo "FAIL unsigned nesh.efi was not refused"
        ok=0
    fi
    if [ $ok = 1 ]; then
        echo "qemu secure boot tests: OK"
    else
        echo "qemu secure boot tests: FAILED (full log: $LOG.txt)"
        exit 1
    fi
    exit 0
fi

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
