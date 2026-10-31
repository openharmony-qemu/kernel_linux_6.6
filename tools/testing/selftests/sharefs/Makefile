# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (c) 2026 Huawei Device Co., Ltd.

# sharefs selftests: sharefs_chmod (RM.001 chmod control "reverse umask"
# with a compile-time fixed 0660 mask).
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

# -Wall -O2 to match the alternative OH-clang recipe documented in
# chmod_fsop.c (catch unused vars / implicit declarations here too).
CFLAGS += -static -Wall -O2

TEST_GEN_PROGS := chmod_fsop

# one-shot board scripts (installed alongside the binaries)
TEST_PROGS := sharedfs_chmod_test.sh

include ../lib.mk
