// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

int main(int argc, char **argv)
{
	struct fiemap fm;
	int rc, fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <file>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	printf("== t_fiemap: FS_IOC_FIEMAP passthrough / graceful EOPNOTSUPP ==\n");
	printf("lower supports it: rc=0 with mapped count; unsupported: errno=95\n\n");

	memset(&fm, 0, sizeof(fm));
	fm.fm_start = 0;
	fm.fm_length = ~0ULL;
	fm.fm_extent_count = 0;
	rc = ioctl(fd, FS_IOC_FIEMAP, &fm);
	printf("FIEMAP(count=0) rc=%d errno=%d (%s) mapped=%u\n",
	       rc, errno, rc ? strerror(errno) : "ok", fm.fm_mapped_extents);
	close(fd);
	return 0;
}
