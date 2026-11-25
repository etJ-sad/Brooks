# SPDX-License-Identifier: GPL-2.0-only
# Brooks build.  Targets:
#   make          kernel ELF  -> build/kernel.elf
#   make img      bootable GPT/UEFI disk image -> build/brooks.img (for dd / Rufus)
#   make esp      the same files as a folder + zip, to copy onto any FAT32 partition
#   make run      boot the image in QEMU + OVMF (serial log on stdout)
#   make smoke    headless QEMU boot test with screenshot (build/screen.ppm)
#   make clean / make distclean

OS_NAME    ?= Brooks
OS_VERSION ?= 0.0.1
IMG_NAME   ?= brooks
BUILD      := build

CXX := clang++
CC  := clang
LD  := ld.lld

LIMINE_DIR := third_party/limine
LIMINE_EFI := $(LIMINE_DIR)/BOOTX64.EFI

GIT_REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)

TARGET_FLAGS := --target=x86_64-unknown-none-elf
CXXFLAGS := $(TARGET_FLAGS) -std=c++20 -O2 -g -pipe -Wall -Wextra -Werror \
	-ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
	-fno-stack-check -fno-pic -fno-pie -fno-lto -fno-omit-frame-pointer \
	-mcmodel=kernel -mno-red-zone -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
	-nostdinc++ -Ikernel/src -Ikernel/include -I$(BUILD)/gen -MMD -MP
ASFLAGS := $(TARGET_FLAGS) -fno-pic -g
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --no-dynamic-linker -T kernel/linker.ld

SRCS_CPP := $(shell find kernel/src -name '*.cpp')
SRCS_S   := $(shell find kernel/src -name '*.S')
OBJS := $(SRCS_CPP:%.cpp=$(BUILD)/%.o) $(SRCS_S:%.S=$(BUILD)/%.S.o)

.PHONY: all img esp run smoke clean distclean limine
all: $(BUILD)/kernel.elf

$(BUILD)/gen/version.h: FORCE
	@mkdir -p $(dir $@)
	@printf '#pragma once\n#define OS_NAME "%s"\n#define OS_VERSION "%s"\n#define OS_BUILD "%s"\n' \
		'$(OS_NAME)' '$(OS_VERSION)' '$(GIT_REV)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
FORCE:

$(BUILD)/%.o: %.cpp $(BUILD)/gen/version.h
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS   $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) kernel/linker.ld
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

NPU_FW := third_party/npu-fw/lib/firmware/intel/vpu/vpu_37xx_v1.bin
$(NPU_FW): scripts/fetch-npu-fw.sh
	./scripts/fetch-npu-fw.sh

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)
$(BUILD)/initrd.tar: $(ROOTFS_FILES) $(NPU_FW)
	@mkdir -p $(BUILD)
	@echo "  TAR  $@"
	@tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00Z' --sort=name \
		-cf $@ -C rootfs . -C ../third_party/npu-fw lib

$(LIMINE_EFI):
	./scripts/fetch-limine.sh

limine: $(LIMINE_EFI)

img: $(BUILD)/$(IMG_NAME).img
$(BUILD)/$(IMG_NAME).img: $(BUILD)/kernel.elf $(BUILD)/initrd.tar $(LIMINE_EFI) $(BUILD)/BOOTX64.EFI boot/limine.conf scripts/make-image.sh
	./scripts/make-image.sh $(BUILD)/kernel.elf $(BUILD)/initrd.tar $@ $(BUILD)/BOOTX64.EFI

esp: $(BUILD)/kernel.elf $(BUILD)/initrd.tar $(LIMINE_EFI) $(BUILD)/BOOTX64.EFI boot/limine.conf
	rm -rf $(BUILD)/esp $(BUILD)/$(IMG_NAME)-esp.zip
	mkdir -p $(BUILD)/esp/EFI/BOOT $(BUILD)/esp/EFI/limine $(BUILD)/esp/boot/limine
	cp $(BUILD)/BOOTX64.EFI $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	cp $(LIMINE_EFI) $(BUILD)/esp/EFI/limine/BOOTX64.EFI
	cp boot/limine.conf $(BUILD)/esp/boot/limine/limine.conf
	cp $(BUILD)/kernel.elf $(BUILD)/esp/boot/kernel.elf
	cp $(BUILD)/initrd.tar $(BUILD)/esp/boot/initrd.tar
	cd $(BUILD)/esp && zip -qr ../$(IMG_NAME)-esp.zip EFI boot
	@echo "ESP files: $(BUILD)/esp  zip: $(BUILD)/$(IMG_NAME)-esp.zip"

smoke: img
	./scripts/qemu-smoke.sh $(BUILD)/$(IMG_NAME).img $(BUILD)/screen.ppm

run: img
	./scripts/run-qemu.sh $(BUILD)/$(IMG_NAME).img

clean:
	rm -rf $(BUILD)
distclean: clean
	rm -rf $(LIMINE_DIR) third_party/npu-fw

-include $(OBJS:.o=.d)

# ---------------------------------------------------------------- Brooks Boot (UEFI loader)
LOADER_FLAGS := --target=x86_64-unknown-windows -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti \
	-fno-stack-protector -fshort-wchar -mno-red-zone -mgeneral-regs-only -fno-builtin -Wall -Wextra -Werror
LOADER_OBJS := $(BUILD)/loader/main.obj $(BUILD)/loader/trampoline.obj

$(BUILD)/loader/main.obj: loader/src/main.cpp loader/src/efi.h kernel/include/limine.h
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(CXX) $(LOADER_FLAGS) -c $< -o $@

$(BUILD)/loader/trampoline.obj: loader/src/trampoline.S
	@mkdir -p $(dir $@)
	@echo "  AS   $<"
	@$(CC) --target=x86_64-unknown-windows -c $< -o $@

$(BUILD)/BOOTX64.EFI: $(LOADER_OBJS)
	@echo "  LINK $@"
	@lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $(LOADER_OBJS) >/dev/null

loader: $(BUILD)/BOOTX64.EFI
.PHONY: loader
