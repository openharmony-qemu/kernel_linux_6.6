// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif

static void read_first(const char *p, char *buf, size_t n)
{
	FILE *f = fopen(p, "r");

	buf[0] = 0;
	if (f) {
		if (!fgets(buf, (int)n, f))
			buf[0] = 0;
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	const char *dir = argv[1];
	char a[256], b[256], ca[16] = {0}, cb[16] = {0};
	long rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <casefold-mount-dir>\n", argv[0]);
		return 2;
	}

	snprintf(a, sizeof(a), "%s/exA.txt", dir);
	snprintf(b, sizeof(b), "%s/exB.txt", dir);

	FILE *fa = fopen(a, "w");

	if (fa) {
		fputs("AAA", fa);
		fclose(fa);
	}
	FILE *fb = fopen(b, "w");

	if (fb) {
		fputs("BBB", fb);
		fclose(fb);
	}

	printf("== t_rename_exchange: rename with non-zero flags must be rejected ==\n");
	printf("RENAME_EXCHANGE(=2) swapping %s and %s, expect -EINVAL\n\n", a, b);

	rc = syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 2);
	printf("renameat2(EXCHANGE) rc=%ld errno=%d(%s)\n", rc, errno, strerror(errno));

	read_first(a, ca, sizeof(ca));
	read_first(b, cb, sizeof(cb));
	printf("content after exchange: %s=%s  %s=%s (must still be AAA / BBB)\n", a, ca, b, cb);

	if (rc == -1 && errno == EINVAL &&
	    strcmp(ca, "AAA") == 0 && strcmp(cb, "BBB") == 0)
		printf("CF-25 PASS: -EINVAL returned and both files unchanged\n");
	else
		printf("CF-25 FAIL: non-zero flags not rejected, or contents changed\n");
	return 0;
}
