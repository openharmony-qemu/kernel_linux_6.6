# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (c) 2026 Huawei Device Co., Ltd.

# sharefs selftests: sharefs_chmod (RM.001 chmod control "reverse umask"
# with a compile-time fixed 0660 mask).
#
# sharefs selftests: sharefs_casefold (ASCII case-insensitive lookup and
# case-only rename) + sharefs_ioctl (ioctl pass-through, fallocate/fiemap,
# fileattr).
#
# Build from the selftests root (or from the kernel tree with
# "make ARCH=arm64 CROSS_COMPILE=<aarch64-prefix-> kselftest-all"):
#   make ARCH=arm64 TARGETS=sharefs CROSS_COMPILE=<aarch64-prefix->
#
# The rk3568 board has a 32-bit musl userspace on an arm64 kernel: a
# statically linked aarch64 binary runs on it fine (no loader involved),
# which is how the board-side scripts deploy this suite. For building
# with the OH clang toolchain instead, see the header of chmod_fsop.c
# (that recipe links dynamically -- a static 32-bit musl binary raises
# SIGILL on the OH image).
#
# The arm32 compat probe is opt-in, same idea as the optional 32-bit
# builds in the x86 selftests (build what the toolchain supports, skip
# and say so otherwise):
#   ... CROSS_COMPILE32=<arm32-prefix->
# Without CROSS_COMPILE32 only t_ioctl32 is left out; the ioctl script
# SKIPs its compat cases with a note instead of failing.

# -Wall -O2 to match the alternative OH-clang recipe documented in
# chmod_fsop.c (catch unused vars / implicit declarations here too).
CFLAGS += -static -Wall -O2

TEST_GEN_PROGS := chmod_fsop t_ioctl t_fiemap t_casefold_rename \
		  t_ci_bench t_rename_exchange t_rename_flags0

# one-shot board scripts + docs (installed alongside the binaries)
TEST_PROGS := sharedfs_chmod_test.sh sharefs_casefold_test.sh \
	      sharefs_ioctl_test.sh
TEST_FILES := README.md

include ../lib.mk

# Optional arm32 binary: same source as t_ioctl.c, second cross toolchain.
ifneq ($(CROSS_COMPILE32),)
CAN_BUILD_32 := $(shell $(CROSS_COMPILE32)gcc -static -fsyntax-only t_ioctl.c >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(CAN_BUILD_32),1)
all: $(OUTPUT)/t_ioctl32
TEST_CUSTOM_PROGS += $(OUTPUT)/t_ioctl32
EXTRA_CLEAN += $(OUTPUT)/t_ioctl32
$(OUTPUT)/t_ioctl32: t_ioctl.c
	$(CROSS_COMPILE32)gcc $(CFLAGS) $(LDFLAGS) -o $@ $<
else
$(warning CROSS_COMPILE32 set but cannot build t_ioctl32 with it - skipping)
endif
endif
