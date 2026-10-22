// SPDX-License-Identifier: GPL-2.0-only
/*
 * chmod_fsop.c - run a single filesystem syscall as a given uid/gid.
 *
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 *
 * Must be run as root (hdc shell on the rk3568 development board); the
 * sharefs selftest drives it with uid 0. Non-zero uids are also supported
 * for ad-hoc identity probes; for those the tool clears the entire
 * capability set, so the caller is a genuinely unprivileged identity even
 * if the parent environment suppresses the setuid() capability fixup (see
 * the capset comment below).
 *
 * Board-side build: the primary artifact is the statically linked
 * aarch64 binary built by this directory's Makefile ("make
 * ARCH=arm64 TARGETS=sharefs" from the selftests root, or any aarch64
 * cross gcc with -static). The rk3568 board has a 32-bit musl
 * userspace on an arm64 kernel; a static binary carries its own libc
 * and runs on it as-is (same deployment as the other sharefs selftest
 * tools).
 *
 * Alternative 32-bit build matching the board userspace (OH clang;
 * link DYNAMICALLY -- a static 32-bit musl binary raises SIGILL on
 * the OH image):
 *   prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang \
 *       --target=arm-linux-ohos \
 *       --sysroot=prebuilts/ohos-sdk/linux/26.0.1/native/sysroot \
 *       -O2 -Wall -o chmod_fsop chmod_fsop.c
 *   <clang's dir>/llvm-strip chmod_fsop   # optional
 *
 * Usage: chmod_fsop <uid> <gid> <op> [args...]
 * Operations:
 *   chmod    <path> <octal mode>
 *   chown    <path> <new uid>
 *   access   <path> <r|w|x|f>
 *   create   <path>
 *   truncate <path> <length>
 *   statuid  <path>          print owner uid
 *   statmode <path>          print permission bits (octal, no leading zero)
 *   setflags <path> <hex>    FS_IOC_SETFLAGS (O_RDONLY, like chattr -i)
 *
 * Exit status: 0 = syscall succeeded, 1 = syscall failed, 2 = usage error,
 *              3 = identity switch failed, 125 = internal error (open/stat)
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* Internal error: unexpected open/stat failure (as opposed to a failure
 * of the syscall under test, which returns 1). */
static void die(const char *msg)
{
	fprintf(stderr, "chmod_fsop: %s: %s\n", msg, strerror(errno));
	exit(125);
}

/* Parse a uid/gid argument strictly (review feedback): reject empty
 * input, trailing garbage and overflow. strtoul() alone returns 0 for
 * junk like "abc", which would silently run the tool as uid 0 (root)
 * and turn negative identity cases into false passes. */
static int parse_id(const char *s, const char *what, unsigned int *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end != '\0' || v > (uid_t)-1) {
		fprintf(stderr, "chmod_fsop: bad %s: '%s'\n", what, s);
		return -1;
	}
	*out = (unsigned int)v;
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int uid;
	unsigned int gid;
	const char *op;

	if (argc < 4) {
		fprintf(stderr, "usage: %s uid gid op args...\n", argv[0]);
		return 2;
	}
	if (parse_id(argv[1], "uid", &uid) || parse_id(argv[2], "gid", &gid))
		return 2;
	op = argv[3];

	if (setgid(gid)) {
		fprintf(stderr, "chmod_fsop: setgid: %s\n", strerror(errno));
		return 3;
	}
	if (setuid(uid)) {
		fprintf(stderr, "chmod_fsop: setuid: %s\n", strerror(errno));
		return 3;
	}

	/*
	 * On OH debug images every init service (including hdcd, i.e. the
	 * whole hdc shell lineage) runs with SECBIT_NO_SETUID_FIXUP_LOCKED
	 * (KeepCapability() in base/startup/init), so setuid() away from
	 * root does not clear the inherited capability set. Identity probes
	 * with a non-zero uid require the caller to be a genuinely
	 * unprivileged identity, so drop the entire capability set here: a
	 * pure drop is always allowed and is not blocked by the LOCKED
	 * securebits. uid 0 keeps its capabilities -- root-side tests rely
	 * on them (e.g. setflags on an immutable file needs
	 * CAP_LINUX_IMMUTABLE).
	 */
	if (uid != 0) {
		struct __user_cap_header_struct hdr = {
			.version = _LINUX_CAPABILITY_VERSION_3,
			.pid = 0,
		};
		struct __user_cap_data_struct data[2] = {};

		/* capset(2) returns EPERM while ambient capabilities are
		 * present: clear them first (best effort; EINVAL means no
		 * ambient capabilities or unsupported by the kernel). */
		if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) &&
		    errno != EINVAL) {
			fprintf(stderr, "chmod_fsop: prctl PR_CAP_AMBIENT: %s\n",
				strerror(errno));
			return 3;
		}
		if (syscall(SYS_capset, &hdr, data)) {
			fprintf(stderr, "chmod_fsop: capset: %s\n", strerror(errno));
			return 3;
		}
	}

	if (!strcmp(op, "chmod") && argc == 6) {
		int r = chmod(argv[4], (mode_t)strtoul(argv[5], NULL, 8));
		int e = errno;

		/* Print the numeric errno so the caller can assert the exact
		 * failure reason (e.g. EPERM=1) instead of accepting any
		 * non-zero rc. */
		printf("ret=%d errno=%s(%d)\n", r, r ? strerror(e) : "-",
		       r ? e : 0);
		return r == 0 ? 0 : 1;
	}
	if (!strcmp(op, "chown") && argc == 6) {
		int r = chown(argv[4], (uid_t)strtoul(argv[5], NULL, 0), (gid_t)-1);

		printf("ret=%d errno=%s\n", r, r ? strerror(errno) : "-");
		return r == 0 ? 0 : 1;
	}
	if (!strcmp(op, "access") && argc == 6) {
		int m = argv[5][0] == 'r' ? R_OK :
			argv[5][0] == 'w' ? W_OK :
			argv[5][0] == 'x' ? X_OK : F_OK;
		int r = access(argv[4], m);

		printf("ret=%d errno=%s\n", r, r ? strerror(errno) : "-");
		return r == 0 ? 0 : 1;
	}
	if (!strcmp(op, "create") && argc == 5) {
		int fd = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0644);

		if (fd >= 0)
			close(fd);
		printf("ret=%d errno=%s\n", fd >= 0 ? 0 : -1,
		       fd >= 0 ? "-" : strerror(errno));
		return fd >= 0 ? 0 : 1;
	}
	if (!strcmp(op, "truncate") && argc == 6) {
		int r = truncate(argv[4], (off_t)strtoull(argv[5], NULL, 0));

		printf("ret=%d errno=%s\n", r, r ? strerror(errno) : "-");
		return r == 0 ? 0 : 1;
	}
	if (!strcmp(op, "statuid") && argc == 5) {
		struct stat st;

		if (stat(argv[4], &st))
			die("stat");
		printf("%lu\n", (unsigned long)st.st_uid);
		return 0;
	}
	if (!strcmp(op, "statmode") && argc == 5) {
		struct stat st;

		if (stat(argv[4], &st))
			die("stat");
		printf("%o\n", st.st_mode & 07777);
		return 0;
	}
	if (!strcmp(op, "setflags") && argc == 6) {
		/* setflags <path> <hex flags> -- use O_RDONLY, like chattr -i:
		 * an IMMUTABLE file refuses a write open, so use a read-only
		 * fd and leave the permission check to the ioctl itself: any
		 * flag change goes through inode_owner_or_capable(); the
		 * IMMUTABLE/APPEND bits additionally require
		 * capable(CAP_LINUX_IMMUTABLE) whether being set or cleared
		 * -- see fileattr_set_prepare() in fs/ioctl.c.
		 */
		int flags = (int)strtoul(argv[5], NULL, 16);
		int fd = open(argv[4], O_RDONLY);
		int r;
		int e;

		if (fd < 0)
			die("open");
		r = ioctl(fd, FS_IOC_SETFLAGS, &flags);
		e = errno; /* save before close() so errno is not clobbered */
		close(fd);
		if (r)
			fprintf(stderr, "chmod_fsop: FS_IOC_SETFLAGS: %s\n",
				strerror(e));
		printf("ret=%d\n", r);
		return r == 0 ? 0 : 1;
	}
	fprintf(stderr, "chmod_fsop: bad op or wrong arg count: '%s'\n", op);
	return 2;
}
