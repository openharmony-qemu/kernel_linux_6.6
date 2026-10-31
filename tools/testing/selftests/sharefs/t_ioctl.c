// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

/*
 * t_ioctl: sharefs ioctl pass-through probe (RM003 v1).
 *
 * One binary covers everything the ioctl script cannot do with
 * toybox alone:
 *   1. a private command 0x12345678 -- unsupported everywhere, so the
 *      errno tells where the call stopped (lower fs -ENOTTY means the
 *      pass-through worked);
 *   2. FS_IOC_GETVERSION (0x80087601) -- plain read ioctl;
 *   3. FS_IOC_GETFLAGS  (0x80086601) -- handled by the VFS via
 *      ->fileattr_get, not by ->unlocked_ioctl;
 *   4. FS_IOC_SETVERSION (0x40087602) -- write-class command, f2fs
 *      does not implement it, so the lower fs error must come back
 *      as-is;
 *   5. the same GETVERSION on a directory fd.
 */
int main(int argc, char **argv)
{
	unsigned long buf = 0xdeadbeef;
	unsigned long ver = 0, old = 0;
	int rc, fd, dfd;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <file> <dir>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open file");
		return 1;
	}

	printf("== t_ioctl: sharefs ioctl pass-through probe ==\n");
	printf("cmd 0x12345678 private -> expect -ENOTTY from lower (unsupported cmd)\n");
	printf("cmd 0x80087601 GETVERSION  -> expect success (passed through)\n");
	printf("cmd 0x80086601 GETFLAGS    -> expect success (fileattr path)\n");
	printf("cmd 0x40087602 SETVERSION  -> f2fs lacks it, error comes back as-is\n\n");

	rc = ioctl(fd, 0x12345678, &buf);
	printf("ioctl(0x12345678 private) rc=%d errno=%d (%s) buf=0x%lx\n",
	       rc, errno, strerror(errno), buf);

	buf = 0xdeadbeef;
	rc = ioctl(fd, 0x80087601, &buf);
	printf("ioctl(0x80087601 GETVER) rc=%d errno=%d (%s) buf=0x%lx\n",
	       rc, errno, rc ? strerror(errno) : "ok", buf);

	buf = 0xdeadbeef;
	rc = ioctl(fd, 0x80086601, &buf);
	printf("ioctl(0x80086601 GETFLG) rc=%d errno=%d (%s) buf=0x%lx\n",
	       rc, errno, rc ? strerror(errno) : "ok", buf);

	rc = ioctl(fd, 0x80087601, &old);
	printf("GETVERSION            rc=%d old=%lu\n", rc, old);

	ver = old + 1;
	rc = ioctl(fd, 0x40087602, &ver);
	printf("SETVERSION(0x40087602) rc=%d errno=%d (%s)\n",
	       rc, errno, rc ? strerror(errno) : "ok");

	rc = ioctl(fd, 0x80087601, &ver);
	printf("GETVERSION again      rc=%d new=%lu %s\n", rc, ver,
	       (rc == 0 && ver == old + 1) ? "CHANGED" : "unchanged");
	close(fd);

	dfd = open(argv[2], O_RDONLY | O_DIRECTORY);
	if (dfd < 0) {
		perror("open dir");
		return 1;
	}
	rc = ioctl(dfd, 0x80087601, &old);
	printf("DIR-fd GETVERSION    rc=%d errno=%d (%s)\n",
	       rc, errno, rc ? strerror(errno) : "ok");
	close(dfd);
	return 0;
}
