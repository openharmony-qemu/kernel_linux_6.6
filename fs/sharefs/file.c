// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/sharefs/file.c
 *
 * Copyright (c) 1998-2022 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2022 Stony Brook University
 * Copyright (c) 2003-2022 The Research Foundation of SUNY
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#include "sharefs.h"
#include "authentication.h"

static int sharefs_readdir(struct file *file, struct dir_context *ctx)
{
	int err;
	struct file *lower_file = NULL;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = sharefs_lower_file(file);
	err = iterate_dir(lower_file, ctx);
	file->f_pos = lower_file->f_pos;
	if (err >= 0)		/* copy the atime */
		fsstack_copy_attr_atime(d_inode(dentry),
					file_inode(lower_file));
	return err;
}

static int sharefs_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_path;
	const struct cred *saved_cred = NULL;
	__u16 perm;

	saved_cred = sharefs_override_file_fsids(inode, &perm);
	if (!saved_cred) {
		err = -ENOMEM;
		return err;
	}

	/* don't open unhashed/deleted files */
	if (d_unhashed(file->f_path.dentry)) {
		err = -ENOENT;
		goto out_err;
	}

	file->private_data =
		kzalloc(sizeof(struct sharefs_file_info), GFP_KERNEL);
	if (!SHAREFS_F(file)) {
		err = -ENOMEM;
		goto out_err;
	}

	/* open lower object and link sharefs's file struct to lower's */
	sharefs_get_lower_path(file->f_path.dentry, &lower_path);
	lower_file = dentry_open(&lower_path, file->f_flags, current_cred());
	path_put(&lower_path);
	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		lower_file = sharefs_lower_file(file);
		if (lower_file) {
			sharefs_set_lower_file(file, NULL);
			fput(lower_file); /* fput calls dput for lower_dentry */
		}
	} else {
		sharefs_set_lower_file(file, lower_file);
	}

	if (err) {
		kfree(SHAREFS_F(file));
	} else {
		kuid_t uid = inode->i_uid;
		kgid_t gid = inode->i_gid;
		mode_t mode = inode->i_mode;
		fsstack_copy_attr_all(inode, sharefs_lower_inode(inode));
		inode->i_uid = uid;
		inode->i_gid = gid;
		inode->i_mode = mode;
	}
out_err:
	sharefs_revert_fsids(saved_cred);
	return err;
}

static int sharefs_flush(struct file *file, fl_owner_t id)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = sharefs_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush) {
		filemap_write_and_wait_range(file->f_mapping, 0, LLONG_MAX);
		err = lower_file->f_op->flush(lower_file, id);
	}

	return err;
}

/* release all lower object references & free the file info structure */
static int sharefs_file_release(struct inode *inode, struct file *file)
{
	struct file *lower_file;

	lower_file = sharefs_lower_file(file);
	if (lower_file) {
		sharefs_set_lower_file(file, NULL);
		fput(lower_file);
	}

	kfree(SHAREFS_F(file));
	return 0;
}

static int sharefs_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	int err;
	struct file *lower_file;
	struct path lower_path;
	struct dentry *dentry = file->f_path.dentry;

	err = __generic_file_fsync(file, start, end, datasync);
	if (err)
		goto out;
	lower_file = sharefs_lower_file(file);
	sharefs_get_lower_path(dentry, &lower_path);
	err = vfs_fsync_range(lower_file, start, end, datasync);
	sharefs_put_lower_path(dentry, &lower_path);
out:
	return err;
}

static int sharefs_fasync(int fd, struct file *file, int flag)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = sharefs_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		err = lower_file->f_op->fasync(fd, lower_file, flag);

	return err;
}

/*
 * Sharefs cannot use generic_file_llseek as ->llseek, because it would
 * only set the offset of the upper file.  So we have to implement our
 * own method to set both the upper and lower file offsets
 * consistently.
 */
static loff_t sharefs_file_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t err;
	struct file *lower_file;

	lower_file = sharefs_lower_file(file);
	lower_file->f_pos = file->f_pos;
	err = generic_file_llseek(lower_file, offset, whence);
	file->f_pos = lower_file->f_pos;

	return err;
}

/*
 * Sharefs read_iter, redirect modified iocb to lower read_iter
 */
ssize_t sharefs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file;

	lower_file = sharefs_lower_file(file);
	if (!lower_file->f_op->read_iter) {
		err = -EINVAL;
		goto out;
	}

	/* prevent lower_file from being released */
	get_file(lower_file);
	iocb->ki_filp = lower_file;
	err = lower_file->f_op->read_iter(iocb, iter);
	iocb->ki_filp = file;
	fput(lower_file);

	/* update upper inode atime as needed */
	if (err >= 0 || err == -EIOCBQUEUED)
		fsstack_copy_attr_atime(d_inode(file->f_path.dentry),
					file_inode(lower_file));
out:
	return err;
}

/*
 * Sharefs write_iter, redirect modified iocb to lower write_iter
 */
ssize_t sharefs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file;

	lower_file = sharefs_lower_file(file);
	if (!lower_file->f_op->write_iter) {
		err = -EINVAL;
		goto out;
	}

	get_file(lower_file); /* prevent lower_file from being released */
	iocb->ki_filp = lower_file;
	err = lower_file->f_op->write_iter(iocb, iter);
	iocb->ki_filp = file;
	fput(lower_file);
	/* update upper inode times/sizes as needed */
	if (err >= 0 || err == -EIOCBQUEUED) {
		fsstack_copy_inode_size(d_inode(file->f_path.dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(file->f_path.dentry),
					file_inode(lower_file));
	}
out:
	return err;
}

int sharefs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	struct file *lower_file;

	lower_file = sharefs_lower_file(file);
	if (!lower_file)
		return -EINVAL;

	if (!lower_file->f_op->mmap)
		return -ENODEV;

	if (WARN_ON(file != vma->vm_file))
		return -EIO;

	vma->vm_file = get_file(lower_file);
	err = call_mmap(vma->vm_file, vma);
	if (err)
		fput(lower_file);
	else
		fput(file);

	file_accessed(file);

	return err;
}

/*
 * pass ioctls through to the lower file with the file owner's
 * credentials.  sharefs does not filter commands; the lower fs's own
 * permission checks (ownership / capable / fd write mode) decide, as
 * they do for a directly opened lower file.
 *
 * The compat path replays what COMPAT_SYSCALL_DEFINE3(ioctl) in
 * fs/ioctl.c does for a plain file, in the same order:
 *   (a) do_vfs_ioctl(f, fd, cmd, compat_ptr(arg)) -- the generic VFS
 *       commands plus ->unlocked_ioctl();
 *   (b) f->f_op->compat_ioctl(f, cmd, arg) -- only reached on
 *       -ENOIOCTLCMD, and with the *raw* 32-bit arg, because a
 *       ->compat_ioctl() handler applies compat_ptr() itself (see
 *       compat_ptr_ioctl(), fs/ioctl.c).
 * do_vfs_ioctl() is static, so (a) is replayed with vfs_ioctl().
 *
 * (b) additionally fires on -ENOTTY, not just on -ENOIOCTLCMD:
 * vfs_ioctl() collapses -ENOIOCTLCMD to -ENOTTY, and 32-bit-only
 * commands such as FS_IOC32_GETVERSION are deliberately left
 * untranslated by the VFS compat entry, so without this the lower fs's
 * 32-bit handler would never run and those commands would fail with
 * -ENOTTY even though the lower fs supports them.
 */
static long sharefs_do_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg, bool compat)
{
	struct file *lower_file;
	const struct cred *saved_cred;
	long err;

	lower_file = sharefs_lower_file(file);
	if (!lower_file)
		return -EINVAL;
	if (!lower_file->f_op)
		return -EOPNOTSUPP;
	if (compat) {
#ifdef CONFIG_COMPAT
		/* either the native or the 32-bit entry point must exist */
		if (!lower_file->f_op->unlocked_ioctl &&
		    !lower_file->f_op->compat_ioctl)
			return -EOPNOTSUPP;
#endif
	} else if (!lower_file->f_op->unlocked_ioctl) {
		return -EOPNOTSUPP;
	}

	saved_cred = sharefs_override_file_fsids(file_inode(file), NULL);
	if (!saved_cred)
		return -ENOMEM;

	if (compat) {
#ifdef CONFIG_COMPAT
		/* (a) native entry point, argument converted as the VFS does */
		err = -ENOIOCTLCMD;
		if (lower_file->f_op->unlocked_ioctl)
			err = vfs_ioctl(lower_file, cmd,
					(unsigned long)compat_ptr(arg));

		/* (b) 32-bit entry point: the VFS hands it the raw arg */
		if ((err == -ENOIOCTLCMD || err == -ENOTTY) &&
		    lower_file->f_op->compat_ioctl)
			err = lower_file->f_op->compat_ioctl(lower_file, cmd,
							     arg);
		if (err == -ENOIOCTLCMD)
			err = -ENOTTY;
#else
		err = -EOPNOTSUPP;
#endif
	} else {
		err = vfs_ioctl(lower_file, cmd, arg);
	}
	sharefs_revert_fsids(saved_cred);

	sharefs_info_ratelimited("ioctl cmd 0x%x, err %ld", cmd, err);
	return err;
}

/* ioctl entry point for 64-bit callers */
static long sharefs_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	return sharefs_do_ioctl(file, cmd, arg, false);
}

/*
 * 32-bit callers go through the same pass-through path.
 * Only built when 32-bit compat syscalls exist (kernel convention,
 * see ext4_compat_ioctl in fs/ext4/ioctl.c).
 *
 * @arg is forwarded raw: that is what the VFS passes to
 * ->compat_ioctl() (fs/ioctl.c), and every handler applies
 * compat_ptr() itself.  compat_ptr() is applied inside
 * sharefs_do_ioctl() only on the ->unlocked_ioctl() path.
 */
#ifdef CONFIG_COMPAT
static long sharefs_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	return sharefs_do_ioctl(file, cmd, arg, true);
}
#endif

/*
 * forward fallocate to the lower file, then sync the new
 * size/blocks into the upper inode (lower changes do not propagate
 * by themselves).
 */
static long sharefs_fallocate(struct file *file, int mode, loff_t offset,
			      loff_t len)
{
	struct inode *inode = file_inode(file);
	struct inode *lower_inode = sharefs_lower_inode(inode);
	struct file *lower_file = sharefs_lower_file(file);
	const struct cred *saved_cred;
	long err;

	if (!lower_inode)
		return -EINVAL;

	/*
	 * Check the upper fd explicitly: vfs_fallocate() only checks
	 * the lower FMODE_WRITE mirrored by sharefs_open().
	 */
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!lower_file)
		return -EINVAL;
	if (!lower_file->f_op || !lower_file->f_op->fallocate)
		return -EOPNOTSUPP;

	saved_cred = sharefs_override_file_fsids(inode, NULL);
	if (!saved_cred)
		return -ENOMEM;

	err = vfs_fallocate(lower_file, mode, offset, len);
	sharefs_revert_fsids(saved_cred);

	if (!err)
		fsstack_copy_inode_size(inode, lower_inode);
	sharefs_info_ratelimited("fallocate mode 0x%x, err %ld",
				 (unsigned int)mode, err);
	return err;
}

const struct file_operations sharefs_main_fops = {
	.llseek		= sharefs_file_llseek,
	.open		= sharefs_open,
	.flush		= sharefs_flush,
	.release	= sharefs_file_release,
	.fsync		= sharefs_fsync,
	.fasync		= sharefs_fasync,
	.read_iter	= sharefs_read_iter,
	.write_iter	= sharefs_write_iter,
	.mmap		= sharefs_file_mmap,
	.splice_read = copy_splice_read,
	.splice_write = iter_file_splice_write,
	.unlocked_ioctl	= sharefs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sharefs_compat_ioctl,
#endif
	.fallocate	= sharefs_fallocate,
};

/* trimmed directory options */
const struct file_operations sharefs_dir_fops = {
	.llseek		= sharefs_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= sharefs_readdir,
	.open		= sharefs_open,
	.release	= sharefs_file_release,
	.flush		= sharefs_flush,
	.fsync		= sharefs_fsync,
	.fasync		= sharefs_fasync,
	.unlocked_ioctl	= sharefs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sharefs_compat_ioctl,
#endif
};
