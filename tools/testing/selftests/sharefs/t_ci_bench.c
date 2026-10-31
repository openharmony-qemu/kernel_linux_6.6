// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	const char *dir = argv[1];
	char p[256];
	int i, fd;
	long t0, t1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <casefold-mount-dir>\n", argv[0]);
		return 2;
	}

	printf("== t_ci_bench: case-insensitive lookup performance baseline (200 opens each)==\n");

	snprintf(p, sizeof(p), "%s/f1.txt", dir);
	fd = open(p, O_RDONLY);
	if (fd < 0) {
		FILE *f = fopen(p, "w");

		if (f) {
			fputs("f1-content", f);
			fclose(f);
		}
	} else {
		close(fd);
	}
	printf("baseline file: %s\n\n", p);

	snprintf(p, sizeof(p), "%s/f1.txt", dir);
	fd = open(p, O_RDONLY);
	if (fd >= 0)
		close(fd);

	t0 = now_ms();
	for (i = 0; i < 200; i++) {
		snprintf(p, sizeof(p), "%s/f1.txt", dir);
		fd = open(p, O_RDONLY);
		if (fd >= 0)
			close(fd);
	}
	t1 = now_ms();
	printf("exact_hit_200x   = %ld ms   (exact hit, no variant scan)\n", t1 - t0);

	t0 = now_ms();
	for (i = 0; i < 200; i++) {
		snprintf(p, sizeof(p), "%s/zzMiss.txt", dir);
		fd = open(p, O_RDONLY);
		if (fd >= 0)
			close(fd);
	}
	t1 = now_ms();
	printf("letters_miss_200x= %ld ms   (miss with letters, triggers variant scan)\n", t1 - t0);

	t0 = now_ms();
	for (i = 0; i < 200; i++) {
		snprintf(p, sizeof(p), "%s/99999999", dir);
		fd = open(p, O_RDONLY);
		if (fd >= 0)
			close(fd);
	}
	t1 = now_ms();
	printf("digits_miss_200x = %ld ms   (digits-only miss, skips variant scan)\n", t1 - t0);

	snprintf(p, sizeof(p), "%s/F1.TXT", dir);
	fd = open(p, O_RDONLY);
	if (fd >= 0) {
		char b[16] = {0};

		read(fd, b, 15);
		printf("F1.TXT hit: %s\n", b);
		close(fd);
	} else {
		printf("F1.TXT open errno=%d(%s)\n", errno, strerror(errno));
	}
	return 0;
}
