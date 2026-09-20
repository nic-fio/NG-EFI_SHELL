# NESH - New EFI Shell
#
#   make            build build/nesh.efi (UEFI x86_64) and build/nesh-host (Linux test build)
#   make test       run the language/command tests with the host build, check the docs
#   make docs       regenerate the command reference of docs/user-manual.html
#   make usb        build/nesh-usb.img, a bootable disk image (dd it to a USB stick)
#   make qemu       boot build/nesh.efi in QEMU/OVMF (interactive, serial console)
#   make qemu-test  run the automated tests inside QEMU
#   make qemu-sbtest  run the Secure Boot tests inside QEMU (own test keys)

CC      ?= gcc
LD      ?= ld
PYTHON  ?= python3
# OVMF firmware image (a single-file OVMF.fd): the first one found, or OVMF=...
OVMF    ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd $(HOME)/Scaricati/OVMF.fd) OVMF.fd)

BUILD   := build

COMMON_SRC := \
	src/lib/util.c \
	src/pal/pal_common.c \
	src/core/con.c src/core/env.c src/core/alias.c src/core/shell.c src/core/lineedit.c src/core/ui.c \
	src/basic/lexer.c src/basic/parser.c src/basic/interp.c src/basic/bfuncs.c \
	src/cmd/cmd_core.c src/cmd/cmd_fs.c src/cmd/cmd_text.c src/cmd/cmd_util.c src/cmd/cmd_edit.c \
	src/lib/eficomp.c

EFI_SRC := $(COMMON_SRC) src/lib/libc.c src/lib/fmt.c src/pal/pal_efi.c \
	$(wildcard src/platform/efi/*.c)

HOST_SRC := $(COMMON_SRC) src/pal/pal_host.c src/platform/host.c

WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

EFI_CFLAGS := -std=gnu11 -O2 $(WARN) -ffreestanding -fno-stack-protector -fno-stack-check \
	-mno-red-zone -fshort-wchar -fPIC -fvisibility=hidden -fno-asynchronous-unwind-tables \
	-fno-builtin -fno-tree-loop-distribute-patterns -fno-strict-aliasing -mgeneral-regs-only \
	-Iinclude
EFI_LDFLAGS := -nostdlib -znocombreloc -shared -Bsymbolic --no-undefined --build-id=none \
	-z noexecstack -T tools/efi.lds

HOST_CFLAGS := -std=gnu11 -O1 -g $(WARN) -DNESH_HOST -Iinclude -fsanitize=address,undefined

EFI_OBJ  := $(EFI_SRC:%.c=$(BUILD)/efi/%.o)
HOST_OBJ := $(HOST_SRC:%.c=$(BUILD)/host/%.o)

all: $(BUILD)/nesh.efi $(BUILD)/nesh-host

$(BUILD)/efi/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(EFI_CFLAGS) -MMD -c $< -o $@

$(BUILD)/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -MMD -c $< -o $@

$(BUILD)/nesh.so: $(EFI_OBJ) tools/efi.lds
	$(LD) $(EFI_LDFLAGS) $(EFI_OBJ) -o $@

$(BUILD)/nesh.efi: $(BUILD)/nesh.so tools/elf2efi.py
	$(PYTHON) tools/elf2efi.py $< $@
	@ls -l $@ | awk '{print "nesh.efi: " $$5 " bytes"}'

$(BUILD)/nesh-host: $(HOST_OBJ)
	$(CC) $(HOST_CFLAGS) $(HOST_OBJ) -o $@

test: $(BUILD)/nesh-host
	@tests/run-host-tests.sh $(BUILD)/nesh-host
	@$(PYTHON) tools/gen-docs.py --check
	@$(PYTHON) tools/check-doc-examples.py $(BUILD)/nesh-host

# regenerate the command reference of the user manual from the help texts
docs:
	@$(PYTHON) tools/gen-docs.py

# bootable USB/disk image: GPT + EFI system partition with NESH and the examples
usb: $(BUILD)/nesh-usb.img
$(BUILD)/nesh-usb.img: $(BUILD)/nesh.efi tools/mkusb.sh $(wildcard examples/*.nsb)
	@tools/mkusb.sh $(BUILD)/nesh.efi $@

qemu: $(BUILD)/nesh.efi
	@tools/run-qemu.sh $(OVMF) $(BUILD)/nesh.efi

# test application for the shell protocol (same toolchain as NESH)
$(BUILD)/tests/shelltest.so: tests/apps/shelltest.c include/efi_shell.h tools/efi.lds
	@mkdir -p $(dir $@)
	$(CC) $(EFI_CFLAGS) -c $< -o $(BUILD)/tests/shelltest.o
	$(LD) $(EFI_LDFLAGS) $(BUILD)/tests/shelltest.o -o $@

$(BUILD)/tests/shelltest.efi: $(BUILD)/tests/shelltest.so
	$(PYTHON) tools/elf2efi.py $< $@

qemu-test: $(BUILD)/nesh.efi $(BUILD)/tests/shelltest.efi
	@tools/run-qemu.sh --test $(OVMF) $(BUILD)/nesh.efi

qemu-nettest: $(BUILD)/nesh.efi
	@tools/run-qemu.sh --nettest $(OVMF) $(BUILD)/nesh.efi

# Secure Boot tests: needs sbsigntool, python3-virt-firmware and an OVMF build
# with Secure Boot support (skipped with a message when they are missing).
qemu-sbtest: $(BUILD)/nesh.efi
	@tools/run-qemu.sh --sbtest $(OVMF) $(BUILD)/nesh.efi

clean:
	rm -rf $(BUILD)

.PHONY: all test docs usb qemu qemu-test qemu-nettest qemu-sbtest clean

-include $(EFI_OBJ:.o=.d) $(HOST_OBJ:.o=.d)
