// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static int n_pass, n_fail;

#define CHECK(cond, name, detail) do {					\
	if (cond) {							\
		n_pass++;						\
		printf("PASS  %s %s\n", name, detail ? detail : "");	\
	} else {							\
		n_fail++;						\
		printf("FAIL  %s %s\n", name, detail ? detail : "");	\
	}								\
} while (0)

static long r2(const char *old, const char *new)
{
	return syscall(SYS_renameat2, AT_FDCWD, old, AT_FDCWD, new, 0);
}

static int exists(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0;
}

int main(int argc, char **argv)
{
	char p[512];
	long rc;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <ci_mount> <sensitive_mount>\n", argv[0]);
		return 2;
	}
	const char *ci = argv[1], *se = argv[2];

	printf("== V1: file case-only rename (direct branch) ==\n");
	snprintf(p, sizeof(p), "%s/v1.txt", ci);
	unlink(p);
	FILE *f = fopen(p, "w");

	if (f) {
		fputs("v1-content", f);
		fclose(f);
	}
	{
		char up[512];

		snprintf(up, sizeof(up), "%s/V1.TXT", ci);
		rc = r2(p, up);
	}
	CHECK(rc == 0, "V1 renameat2 rc==0", rc ? strerror(errno) : "");
	snprintf(p, sizeof(p), "%s/V1.TXT", ci);
	CHECK(exists(p), "V1 upper new name visible", "");
	{
		char low[512], buf[64] = {0};

		snprintf(low, sizeof(low), "%s/v1.txt", ci);
		FILE *g = fopen(low, "r");

		if (g) {
			size_t n = fread(buf, 1, sizeof(buf)-1, g);

			fclose(g);
			(void)n;
		}
		CHECK(strncmp(buf, "v1-content", 10) == 0,
		      "V1 read back via lower-case variant", buf);
	}

	printf("== V2: directory case-only rename (direct branch) ==\n");
	snprintf(p, sizeof(p), "%s/v2dir", ci);
	rmdir(p);
	rc = mkdir(p, 0755);
	{
		char up[512];

		snprintf(up, sizeof(up), "%s/V2DIR", ci);
		rc = r2(p, up);
	}
	CHECK(rc == 0, "V2 renameat2 dir rc==0", "");
	snprintf(p, sizeof(p), "%s/V2DIR", ci);
	CHECK(exists(p), "V2 upper new dir visible", "");

	printf("== V3: same-name different inode -> -EEXIST ==\n");
	{
		char a[512], b[512];

		snprintf(a, sizeof(a), "%s/v3.txt", ci);
		snprintf(b, sizeof(b), "%s/V3.txt", ci);
		unlink(a); unlink(b);
		f = fopen(a, "w");
		if (f) {
			fputs("lower-a", f);
			fclose(f);
		}
		f = fopen(b, "w");
		if (f) {
			fputs("upper-A", f);
			fclose(f);
		}
		rc = r2(a, b);
		CHECK(rc == -1 && errno == EEXIST, "V3 expect errno==EEXIST(17)",
		      strerror(errno));
	}

	printf("== V4: non-case rename (a->b) -> standard path ==\n");
	{
		char a[512], b[512];

		snprintf(a, sizeof(a), "%s/v4a.txt", ci);
		snprintf(b, sizeof(b), "%s/v4b.txt", ci);
		unlink(a); unlink(b);
		f = fopen(a, "w");
		if (f)
			fclose(f);
		rc = r2(a, b);
		CHECK(rc == 0, "V4 rename rc==0", "");
		CHECK(exists(b) && !exists(a), "V4 moved", "");
	}

	printf("== V5: cross-dir rename -> standard path ==\n");
	{
		char d1[512], d2[512], s[512], t[512];

		snprintf(d1, sizeof(d1), "%s/v5d1", ci);
		snprintf(d2, sizeof(d2), "%s/v5d2", ci);
		mkdir(d1, 0755); mkdir(d2, 0755);
		snprintf(s, sizeof(s), "%s/x.txt", d1);
		snprintf(t, sizeof(t), "%s/x.txt", d2);
		f = fopen(s, "w");
		if (f)
			fclose(f);
		rc = r2(s, t);
		CHECK(rc == 0, "V5 cross-dir rc==0", "");
		CHECK(exists(t), "V5 landed", "");
	}

	printf("== V6: sensitive mount -> block not entered ==\n");
	{
		char lo[512], up[512];

		snprintf(lo, sizeof(lo), "%s/v6.txt", se);
		snprintf(up, sizeof(up), "%s/V6.TXT", se);
		unlink(lo); unlink(up);
		f = fopen(lo, "w");
		if (f) {
			fputs("v6", f);
			fclose(f);
		}
		rc = r2(lo, up);
		CHECK(rc == 0, "V6 sensitive rename rc==0 (standard path)", "");
		CHECK(exists(up), "V6 new name exists", "");
	}

	printf("\n== SUMMARY: %d pass, %d fail ==\n", n_pass, n_fail);
	printf("\n== manual dmesg confirmation (run on board) ==\n");
	printf("  dmesg | grep 'casefold rename (direct)' | tail -4   # expect >=2 lines (V1,V2), err 0\n");
	printf("  dmesg | grep 'casefold rename target exists' | tail -2  # expect 1 line (V3)\n");
	printf("  dmesg | grep -c 'casefold rename'   # V4/V5/V6 must NOT add casefold lines\n");
	return n_fail ? 1 : 0;
}
