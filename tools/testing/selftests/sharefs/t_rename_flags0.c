// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif

static int n_pass, n_fail;

#define CHECK(cond, name) do {			\
	if (cond) {				\
		n_pass++;			\
		printf("PASS  %s\n", name);	\
	} else {				\
		n_fail++;			\
		printf("FAIL  %s\n", name);	\
	}					\
} while (0)

static int exists(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0;
}

int main(int argc, char **argv)
{
	const char *dir = argv[1];
	char src[256], dst[256];
	long rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <casefold-mount-dir>\n", argv[0]);
		return 2;
	}

	printf("== t_rename_flags0: flags=0 rename regression (dir + file)==\n\n");

	snprintf(src, sizeof(src), "%s/wdir", dir);
	snprintf(dst, sizeof(dst), "%s/WDIR", dir);
	rmdir(dst);
	mkdir(src, 0755);

	printf("1) directory %s -> %s\n", src, dst);
	rc = syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 0);
	if (rc == 0)
		printf("   renameat2(flags=0) rc=0\n");
	else
		printf("   renameat2(flags=0) rc=%ld errno=%d(%s)\n", rc, errno, strerror(errno));
	CHECK(rc == 0, "CF-24 dir rename(flags=0) rc=0");
	CHECK(exists(dst), "CF-24 dir new name visible");

	if (rc == 0) {
		rc = syscall(SYS_renameat2, AT_FDCWD, dst, AT_FDCWD, src, 0);
		printf("   rename back rc=%ld\n", rc);
	}

	snprintf(src, sizeof(src), "%s/wf.txt", dir);
	snprintf(dst, sizeof(dst), "%s/WF.TXT", dir);
	unlink(dst);
	FILE *f = fopen(src, "w");

	if (f) {
		fputs("w", f);
		fclose(f);
	}

	printf("2) file %s -> %s\n", src, dst);
	rc = syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 0);
	if (rc == 0)
		printf("   renameat2(flags=0) rc=0\n");
	else
		printf("   renameat2(flags=0) rc=%ld errno=%d(%s)\n", rc, errno, strerror(errno));
	CHECK(rc == 0, "CF-24 file rename(flags=0) rc=0");
	CHECK(exists(dst), "CF-24 file new name visible");

	printf("\nSUMMARY: %d pass, %d fail\n", n_pass, n_fail);
	return n_fail ? 1 : 0;
}
