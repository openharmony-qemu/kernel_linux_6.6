// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/sharefs/inode.c
 *
 * Copyright (c) 1998-2022 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2022 Stony Brook University
 * Copyright (c) 2003-2022 The Research Foundation of SUNY
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#include <linux/fs.h>
#include <linux/fileattr.h>
#include <linux/fiemap.h>
#include <linux/random.h>
#include "sharefs.h"
#include "authentication.h"

static int sharefs_getattr(struct mnt_idmap *idmap, const struct path *path, struct kstat *stat, 
                          u32 request_mask, unsigned int flags)
{
	struct path lower_path;
	int ret;

	sharefs_get_lower_path(path->dentry, &lower_path);
	ret = vfs_getattr_nosec(&lower_path, stat, request_mask, flags);
	stat->ino = d_inode(path->dentry)->i_ino;
	stat->uid = d_inode(path->dentry)->i_uid;
	stat->gid = d_inode(path->dentry)->i_gid;
	stat->mode = d_inode(path->dentry)->i_mode;
	stat->dev = 0;
	stat->rdev = 0;
	sharefs_put_lower_path(path->dentry, &lower_path);

	return ret;
}

static ssize_t sharefs_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;

	sharefs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!(d_inode(lower_dentry)->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}
	err = vfs_listxattr(lower_dentry, buffer, buffer_size);
	if (err)
		goto out;
	fsstack_copy_attr_atime(d_inode(dentry),
				d_inode(lower_path.dentry));
out:
	sharefs_put_lower_path(dentry, &lower_path);
	return err;
}

static int sharefs_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
{
	struct sharefs_sb_info *sbi = SHAREFS_SB(inode->i_sb);
	if (sbi->override_support_delete) {
		return 0;
	}
	unsigned short mode = sharefs_lower_inode(inode)->i_mode;
	kuid_t cur_uid = current_fsuid();
	if (uid_eq(cur_uid, ROOT_UID))
		return 0;
	if (uid_eq(cur_uid, inode->i_uid)) {
		mode >>= 6;
	} else if (in_group_p(inode->i_gid)) {
		mode >>= 3;
	}

	if ((mask & ~mode & (MAY_READ | MAY_WRITE | MAY_EXEC)) == 0)
		return 0;

	return -EACCES;
}

static int sharefs_override_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
{
	unsigned short mode = sharefs_lower_inode(inode)->i_mode;
	mode >>= 6;
	kuid_t cur_uid = current_fsuid();
	if (uid_eq(cur_uid, ROOT_UID)) {
		return 0;
	}

	if ((mask & ~mode & (MAY_READ | MAY_WRITE | MAY_EXEC)) == 0) {
		return 0;
	}
	return -EACCES;
}

static int sharefs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
			 umode_t mode, bool want_excl)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;
	const struct cred *saved_cred = NULL;
	__u16 child_perm;

	saved_cred = sharefs_override_file_fsids(dir, &child_perm);
	if (!saved_cred) {
		err = -ENOMEM;
		return err;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);
	err = vfs_create(&nop_mnt_idmap, d_inode(lower_parent_dentry), lower_dentry, mode,
			 want_excl);
	if (err)
		goto out;
	err = sharefs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, sharefs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
	return err;
}

static int sharefs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, const char *symname)
{
	struct dentry *lower_parent_dentry;
	struct path lower_path;
	const struct cred *saved_cred;
	int err;

	sharefs_info("create symlink name=%pd", dentry);
	saved_cred = sharefs_override_file_fsids(dir, NULL);
	if (!saved_cred) {
		err = -ENOMEM;
		goto out_err;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	err = mnt_want_write(lower_path.mnt);
	if (err)
		goto out_put;

	lower_parent_dentry = lock_parent(lower_path.dentry);
	err = vfs_symlink(&nop_mnt_idmap, d_inode(lower_parent_dentry),
			  lower_path.dentry, symname);
	if (err)
		goto out;

	err = sharefs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;

	SHAREFS_I(d_inode(dentry))->perm =
		perm_get_next_level_override(SHAREFS_I(dir)->perm);
	fsstack_copy_attr_times(dir, sharefs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	mnt_drop_write(lower_path.mnt);
out_put:
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
out_err:
	return err;
}

static const char *sharefs_get_link(struct dentry *dentry, struct inode *inode,
				    struct delayed_call *done)
{
	struct path lower_path;
	const struct cred *saved_cred;
	const char *link;

	/* Accessing the lower path and overriding credentials can block. */
	if (!dentry)
		return ERR_PTR(-ECHILD);

	sharefs_info("read symlink name=%pd", dentry);
	saved_cred = sharefs_override_file_fsids(inode, NULL);
	if (!saved_cred) {
		link = ERR_PTR(-ENOMEM);
		goto out_err;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	/* Let the VFS release the lower link body after it has finished using it. */
	link = vfs_get_link(lower_path.dentry, done);
	if (!IS_ERR(link))
		fsstack_copy_attr_atime(inode, d_inode(lower_path.dentry));
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
out_err:
	return link;
}

static int sharefs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;
	const struct cred *saved_cred = NULL;
	__u16 child_perm;

	saved_cred = sharefs_override_file_fsids(dir, &child_perm);
	if (!saved_cred) {
		err = -ENOMEM;
		return err;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);
	err = vfs_mkdir(&nop_mnt_idmap, d_inode(lower_parent_dentry), lower_dentry, mode);
	if (err)
		goto out;

	err = sharefs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, sharefs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));
	/* update number of links on parent directory */
	set_nlink(dir, sharefs_lower_inode(dir)->i_nlink);

out:
	unlock_dir(lower_parent_dentry);
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
	return err;
}

static int sharefs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dentry = NULL;
	struct inode *lower_dir_inode = sharefs_lower_inode(dir);
	struct dentry *lower_dir_dentry = NULL;
	struct path lower_path;
	const struct cred *saved_cred = NULL;
	__u16 child_perm;

	saved_cred = sharefs_override_file_fsids(dir, &child_perm);
	if (!saved_cred) {
			err = -ENOMEM;
			return err;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	dget(lower_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	err = vfs_unlink(&nop_mnt_idmap, lower_dir_inode, lower_dentry, NULL);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, lower_dir_inode);
	fsstack_copy_inode_size(dir, lower_dir_inode);
	set_nlink(dentry->d_inode,
		  sharefs_lower_inode(dentry->d_inode)->i_nlink);
	dentry->d_inode->__i_ctime = dir->__i_ctime;
	d_drop(dentry);

out:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
	return err;
}

static int sharefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	struct path lower_path;
	const struct cred *saved_cred = NULL;
	__u16 child_perm;
	
	saved_cred = sharefs_override_file_fsids(dir, &child_perm);
	if (!saved_cred) {
		err = -ENOMEM;
		return err;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_dir_dentry = lock_parent(lower_dentry);
	err = vfs_rmdir(&nop_mnt_idmap, lower_dir_dentry->d_inode, lower_dentry);
	if (err)
		goto out;

	d_drop(dentry);
	if (dentry->d_inode)
		clear_nlink(dentry->d_inode);
	fsstack_copy_attr_times(dir, lower_dir_dentry->d_inode);
	fsstack_copy_inode_size(dir, lower_dir_dentry->d_inode);
	set_nlink(dir, lower_dir_dentry->d_inode->i_nlink);

out:
	unlock_dir(lower_dir_dentry);
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
	return err;
}

static int sharefs_check_lower_rename(struct super_block *old_sb, struct super_block *new_sb)
{
	if (old_sb->s_id != new_sb->s_id) {
		return -EXDEV;
	} 

	return 0;
}

/* case-only rename */

/*
 * lookup that guarantees the returned dentry carries the exact
 * requested spelling.  On a CI lower a plain lookup may return a cached
 * dentry whose name only CI-matches (a case variant); renaming onto it
 * would write the variant spelling to disk.  Drop such a hit and retry
 * so a fresh dentry with the exact spelling is used.
 */
static struct dentry *sharefs_ci_lookup_exact(const struct qstr *name,
					      struct dentry *parent)
{
	struct dentry *d;

	d = lookup_one_qstr_excl(name, parent,
			LOOKUP_RENAME_TARGET | REVALIDATE_FOR_CASEFOLD_RENAME);
	if (IS_ERR(d))
		return d;
	if (d->d_name.len == name->len &&
	    !memcmp(d->d_name.name, name->name, name->len))
		return d;
	d_drop(d);
	dput(d);
	return lookup_one_qstr_excl(name, parent,
			LOOKUP_RENAME_TARGET | REVALIDATE_FOR_CASEFOLD_RENAME);
}

/*
 * two-step rename for CI-native lowers: old name -> temp name ->
 * new name.  The lower fs treats "a.txt" and "A.TXT" as the same name,
 * so a direct rename would be swallowed by its same-name check.  The
 * caller holds the lower parent's exclusive lock, so
 * lookup_one_qstr_excl() is safe here.
 */
static int sharefs_case_rename_two_step(struct inode *lower_dir,
					struct dentry *lower_dir_dentry,
					struct dentry *lower_old_dentry,
					const struct qstr *orig_upper_name,
					const struct qstr *new_name)
{
	struct dentry *tmp_dentry = NULL, *lower_new_dentry = NULL;
	struct dentry *check_dentry = NULL, *lower_orig_dentry = NULL;
	struct renamedata rd;
	char tmp_name[64];
	struct qstr tmp_qstr;
	int err, rb_err;

	/* random suffix so other processes cannot pre-create the temp name */
	snprintf(tmp_name, sizeof(tmp_name), ".sharefs_ci_tmp_%lu_%08x",
		 (unsigned long)jiffies, get_random_u32());
	tmp_qstr = (struct qstr)QSTR_INIT(tmp_name, strlen(tmp_name));

	/*
	 * Rollback target name is the SHAREFS dentry name (caller-passed),
	 * not the lower dentry name: with a CI lower the lower dentry can
	 * be bound to a case-variant spelling during path lookup, and only
	 * the upper name preserves the user-visible original spelling.
	 */

	/* skip the sharefs CI fallback to avoid recursion */
	tmp_dentry = sharefs_ci_lookup_exact(&tmp_qstr, lower_dir_dentry);
	if (IS_ERR(tmp_dentry)) {
		err = PTR_ERR(tmp_dentry);
		tmp_dentry = NULL;
		goto out;
	}

	/* the temp name must be free: give up on collision */
	if (!d_is_negative(tmp_dentry)) {
		err = -EEXIST;
		goto out;
	}

	/*
	 * step 1: old name -> temp name.  d_move() moves the temp name
	 * onto lower_old_dentry, so the inode keeps travelling with it;
	 * tmp_dentry is negative afterwards and must not be used as a
	 * rename source again.
	 */
	memset(&rd, 0, sizeof(rd));
	rd.old_mnt_idmap = &nop_mnt_idmap;
	rd.old_dir   = lower_dir;
	rd.old_dentry = lower_old_dentry;
	rd.new_mnt_idmap = &nop_mnt_idmap;
	rd.new_dir   = lower_dir;
	rd.new_dentry = tmp_dentry;
	err = vfs_rename(&rd);
	if (err)
		goto out;

	/* step 2: temp name (still on lower_old_dentry) -> new name */
	lower_new_dentry = sharefs_ci_lookup_exact(new_name, lower_dir_dentry);
	if (IS_ERR(lower_new_dentry)) {
		err = PTR_ERR(lower_new_dentry);
		lower_new_dentry = NULL;
		goto rollback;
	}

	memset(&rd, 0, sizeof(rd));
	rd.old_mnt_idmap = &nop_mnt_idmap;
	rd.old_dir   = lower_dir;
	rd.old_dentry = lower_old_dentry;
	rd.new_mnt_idmap = &nop_mnt_idmap;
	rd.new_dir   = lower_dir;
	rd.new_dentry = lower_new_dentry;
	err = vfs_rename(&rd);
	if (err)
		goto rollback;

	/*
	 * Post-check: the new name must resolve to the source inode.
	 * Anything else means the steps misfired; undo to be safe.
	 */
	check_dentry = sharefs_ci_lookup_exact(new_name, lower_dir_dentry);
	if (IS_ERR(check_dentry)) {
		err = PTR_ERR(check_dentry);
		check_dentry = NULL;
		goto rollback;
	}
	if (d_is_negative(check_dentry) ||
	    d_inode(check_dentry) != d_inode(lower_old_dentry)) {
		sharefs_warning("case rename verify failed for '%s'",
				new_name->name);
		err = -EIO;
		goto rollback;
	}
	goto out;

rollback:
	/*
	 * Drop the new-name dentry first: on a CI lower its negative
	 * dentry CI-matches the original name, so the lookup below
	 * would hit it and the rollback would rename to the NEW
	 * spelling instead of the original one.
	 */
	if (lower_new_dentry)
		d_drop(lower_new_dentry);
	/*
	 * Best effort: move the file (still carried by lower_old_dentry)
	 * back to the saved original name.  If this fails too the file
	 * stays at the temp name; the failure is logged so it stays
	 * visible.
	 */
	lower_orig_dentry = sharefs_ci_lookup_exact(orig_upper_name,
			lower_dir_dentry);
	if (IS_ERR(lower_orig_dentry)) {
		rb_err = PTR_ERR(lower_orig_dentry);
		lower_orig_dentry = NULL;
	} else {
		memset(&rd, 0, sizeof(rd));
		rd.old_mnt_idmap = &nop_mnt_idmap;
		rd.old_dir   = lower_dir;
		rd.old_dentry = lower_old_dentry;
		rd.new_mnt_idmap = &nop_mnt_idmap;
		rd.new_dir   = lower_dir;
		rd.new_dentry = lower_orig_dentry;
		rb_err = vfs_rename(&rd);
	}
	if (rb_err) {
		sharefs_warning("case rename rollback failed, file left at '%s', err %d",
				tmp_name, rb_err);
		/*
		 * Rollback failed too: the file is stranded under the temp
		 * name, so @err no longer describes the state (it claims
		 * nothing moved).  Report -EIO instead.
		 */
		err = -EIO;
	} else {
		sharefs_info("case rename rollback ok, orig err %d", err);
	}

out:
	dput(check_dentry);
	dput(lower_orig_dentry);
	dput(lower_new_dentry);
	dput(tmp_dentry);
	return err;
}

/*
 * rename that changes only the case of a name.
 *
 * Returns 0 when handled (success or a true no-op), -EXDEV when this
 * is not a case-only rename (different parent, different length, or
 * no ASCII letters) and the standard rename path should be used, or
 * another negative errno on real errors.
 *
 * Not wired into the default ->rename call chain: the VFS
 * source==target shortcut (fs/namei.c) returns 0 before reaching
 * ->rename, so making this effective end to end requires a VFS core
 * change.  Interface-level testing builds with
 * SHAREFS_CASEFOLD_RENAME_EXPERIMENTAL (see Makefile).
 */
int sharefs_casefold_rename(struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
{
	struct path lower_old_path;
	struct dentry *lower_old_dentry, *lower_dir_dentry;
	struct dentry *lower_probe;
	struct inode *lower_dir;
	struct renamedata rd;
	const struct cred *saved_cred = NULL;
	int err;

	/* same-directory only */
	if (old_dir != new_dir)
		return -EXDEV;

	/* true no-op: byte-identical names */
	if (old_dentry->d_name.len == new_dentry->d_name.len &&
	    !memcmp(old_dentry->d_name.name, new_dentry->d_name.name,
		    old_dentry->d_name.len))
		return 0;

	/* different lengths or no letters: not case-only */
	if (old_dentry->d_name.len != new_dentry->d_name.len)
		return -EXDEV;
	if (!sharefs_name_needs_ci(&new_dentry->d_name))
		return -EXDEV;

	sharefs_get_lower_path(old_dentry, &lower_old_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_dir_dentry = dget_parent(lower_old_dentry);
	lower_dir = d_inode(lower_dir_dentry);

	/*
	 * The upper new_dentry is deliberately negative, so look the
	 * new name up in the lower fs ourselves; the caller already
	 * holds the parent locks (we sit between lock_rename() and
	 * vfs_rename()).
	 */
	lower_probe = lookup_one_qstr_excl(&new_dentry->d_name, lower_dir_dentry,
			LOOKUP_RENAME_TARGET | REVALIDATE_FOR_CASEFOLD_RENAME);
	if (IS_ERR(lower_probe)) {
		err = PTR_ERR(lower_probe);
		goto out;
	}

	saved_cred = sharefs_override_file_fsids(old_dir, NULL);
	if (!saved_cred) {
		err = -ENOMEM;
		goto out_put_probe;
	}

	if (d_is_negative(lower_probe)) {
		/* case-sensitive lower: one plain vfs_rename */
		memset(&rd, 0, sizeof(rd));
		rd.old_mnt_idmap = &nop_mnt_idmap;
		rd.old_dir   = lower_dir;
		rd.old_dentry = lower_old_dentry;
		rd.new_mnt_idmap = &nop_mnt_idmap;
		rd.new_dir   = lower_dir;
		rd.new_dentry = lower_probe;
		err = vfs_rename(&rd);
		sharefs_info("casefold rename (direct), err %d", err);
	} else if (d_inode(lower_probe) == d_inode(lower_old_dentry)) {
		/* CI-native lower: new name hits the source inode, two steps */
		err = sharefs_case_rename_two_step(lower_dir, lower_dir_dentry,
						   lower_old_dentry,
						   &old_dentry->d_name,
						   &new_dentry->d_name);
		sharefs_info("casefold rename (two-step), err %d", err);
	} else {
		/* another inode already owns a case variant of the new name */
		err = -EEXIST;
		sharefs_warning("casefold rename target exists, err -EEXIST");
	}

	if (err)
		goto out_revert;

	/*
	 * Leave the upper dentry cache alone: sharefs_rename()'s
	 * vfs_rename() d_move()s it, and manual d_drop here would leave
	 * stale negatives below.  Revalidation is covered by
	 * sharefs_d_revalidate() returning 0.
	 */
	fsstack_copy_attr_all(new_dir, lower_dir);
	fsstack_copy_inode_size(new_dir, lower_dir);

out_revert:
	sharefs_revert_fsids(saved_cred);
out_put_probe:
	dput(lower_probe);
out:
	dput(lower_dir_dentry);
	sharefs_put_lower_path(old_dentry, &lower_old_path);
	return err;
}

static int sharefs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
				struct inode *new_dir, struct dentry *new_dentry,
				unsigned int flags)
{
	int err;
	struct dentry *lower_old_dentry = NULL;
	struct dentry *lower_new_dentry = NULL;
	struct dentry *lower_old_dir_dentry = NULL;
	struct dentry *lower_new_dir_dentry = NULL;
	struct dentry *trap = NULL;
	struct path lower_old_path, lower_new_path;
	struct renamedata rename_data;
	const struct cred *saved_cred = NULL;
	__u16 child_perm;

	if (flags)
		return -EINVAL;

	saved_cred = sharefs_override_file_fsids(old_dir, &child_perm);
	if (!saved_cred) {
		err = -ENOMEM;
		return err;
	}

	sharefs_get_lower_path(old_dentry, &lower_old_path);
	sharefs_get_lower_path(new_dentry, &lower_new_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_new_dentry = lower_new_path.dentry;
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);
	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	/* source should not be ancestor of target */
	if (trap == lower_old_dentry) {
		err = -EINVAL;
		goto out;
	}
	/* target should not be ancestor of source */
	if (trap == lower_new_dentry) {
		err = -ENOTEMPTY;
		goto out;
	}

#ifdef SHAREFS_CASEFOLD_RENAME_EXPERIMENTAL
	/* test-only hook; keep it undefined in production builds */
	if (old_dir == new_dir &&
	    !SHAREFS_SB(old_dir->i_sb)->case_sensitive) {
		err = sharefs_casefold_rename(old_dir, old_dentry, new_dir,
					      new_dentry);
		if (err != -EXDEV)
			goto out;
	}
#endif

	rename_data.old_mnt_idmap  = &nop_mnt_idmap;
	rename_data.old_dir = lower_old_dir_dentry->d_inode;
	rename_data.old_dentry = lower_old_dentry;
	rename_data.new_mnt_idmap = &nop_mnt_idmap;
	rename_data.new_dir = lower_new_dir_dentry->d_inode;
	rename_data.new_dentry = lower_new_dentry;
	rename_data.flags = flags;

	err = sharefs_check_lower_rename(lower_old_path.mnt->mnt_sb, lower_new_path.mnt->mnt_sb);
	if (err != 0) {
		goto out;
	}
	err = vfs_rename(&rename_data);
	if (err)
		goto out;

	fsstack_copy_attr_all(new_dir, lower_new_dir_dentry->d_inode);
	fsstack_copy_inode_size(new_dir, lower_new_dir_dentry->d_inode);
	if (new_dir != old_dir) {
		fsstack_copy_attr_all(old_dir,
				    lower_old_dir_dentry->d_inode);
		fsstack_copy_inode_size(old_dir,
					lower_old_dir_dentry->d_inode);
	}

out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	sharefs_put_lower_path(old_dentry, &lower_old_path);
	sharefs_put_lower_path(new_dentry, &lower_new_path);
	sharefs_revert_fsids(saved_cred);
	return err;
}

static int sharefs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *ia)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *inode;
	struct inode *lower_inode;
	struct path lower_path;
	struct iattr lower_ia;
	
	inode = dentry->d_inode;
	/*
	 * Check if user has permission to change inode.  We don't check if
	 * this user can change the lower inode: that should happen when
	 * calling notify_change on the lower inode.
	 */
 	const struct cred *saved_cred = NULL;
 	__u16 child_perm;
 	 
	saved_cred = sharefs_override_file_fsids(inode, &child_perm);
	if (!saved_cred) {
		err = -ENOMEM;
		goto out_err;
	}

	err = setattr_prepare(&nop_mnt_idmap, dentry, ia);
	if (err)
		goto out_err;

	sharefs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_inode = sharefs_lower_inode(inode);

	/* prepare our own lower struct iattr (with the lower file) */
	memcpy(&lower_ia, ia, sizeof(lower_ia));
	if (ia->ia_valid & ATTR_FILE)
		lower_ia.ia_file = sharefs_lower_file(ia->ia_file);

	/*
	 * If shrinking, first truncate upper level to cancel writing dirty
	 * pages beyond the new eof; and also if its' maxbytes is more
	 * limiting (fail with -EFBIG before making any change to the lower
	 * level).  There is no need to vmtruncate the upper level
	 * afterwards in the other cases: we fsstack_copy_inode_size from
	 * the lower level.
	 */
	if (ia->ia_valid & ATTR_SIZE) {
		err = inode_newsize_ok(inode, ia->ia_size);
		if (err)
			goto out;
		truncate_setsize(inode, ia->ia_size);
	}

	/*
	 * chmod control ("reverse umask"), semantics fixed in the
	 * requirement alignment:
	 * - the u/g r/w bits are force OR-ed with sbi->share_perm_mask
	 *   (pure OR: bits set in the mask cannot be cleared by chmod);
	 * - u/g x bits and other-class bits follow the chmod request
	 *   (directories are allowed to be locked by chmod);
	 * - suid/sgid/sticky bits are dropped, never passed through;
	 * - chown (ATTR_UID/ATTR_GID) is still silently dropped as before
	 *   (stripped from lower_ia below, so chown() itself returns 0).
	 *
	 * Ordering contract (review-confirmed, do not reorder):
	 * - the request is read from ia->ia_mode AFTER setattr_prepare()
	 *   above -- prepare() may adjust the request (e.g. strip S_ISGID
	 *   via in_group_or_capable) and those adjustments are inherited;
	 *   prepare() never rejects a MODE change for sgid+x reasons (it
	 *   strips instead), so no legitimate chmod gets refused here;
	 * - lower_ia.ia_mode computed here is final -- nothing below
	 *   modifies it before notify_change().
	 */
	if (ia->ia_valid & ATTR_MODE) {
		unsigned int perm_mask = SHAREFS_SB(inode->i_sb)->share_perm_mask;

		lower_ia.ia_mode = (ia->ia_mode & ~(S_ISUID | S_ISGID | S_ISVTX)) |
				   perm_mask;
		lower_ia.ia_valid |= ATTR_MODE;
	}

	lower_ia.ia_valid &= ~(ATTR_UID | ATTR_GID);

	/* notify the (possibly copied-up) lower inode */
	/*
	 * Note: we use lower_dentry->d_inode, because lower_inode may be
	 * unlinked (no inode->i_sb and i_ino==0.  This happens if someone
	 * tries to open(), unlink(), then ftruncate() a file.
	 */

	inode_lock(d_inode(lower_dentry));
	err = notify_change(&nop_mnt_idmap, lower_dentry, &lower_ia, /* note: lower_ia */
			    NULL);
	inode_unlock(d_inode(lower_dentry));

	if (err)
		goto out;

	/* get attributes from the lower inode */
	fsstack_copy_attr_all(inode, lower_inode);
	/*
	 * Not running fsstack_copy_inode_size(inode, lower_inode), because
	 * VFS should update our inode size, and notify_change on
	 * lower_inode should update its size.
	 */

out:
	sharefs_put_lower_path(dentry, &lower_path);
	sharefs_revert_fsids(saved_cred);
out_err:
	return err;
}

/*
 * forward FIEMAP to the lower inode.  ioctl_fiemap() has
 * already run fiemap_prep() and set up the user extent buffer, so
 * just pass fieinfo through unchanged.  tmpfs/vfat/exfat have no
 * ->fiemap: report -EOPNOTSUPP instead of crashing.
 */
static int sharefs_fiemap(struct inode *inode,
			  struct fiemap_extent_info *fieinfo,
			  u64 start, u64 len)
{
	struct inode *lower_inode = sharefs_lower_inode(inode);
	const struct cred *saved_cred;
	int err;

	if (!lower_inode || !lower_inode->i_op ||
	    !lower_inode->i_op->fiemap)
		return -EOPNOTSUPP;

	saved_cred = sharefs_override_file_fsids(inode, NULL);
	if (!saved_cred)
		return -ENOMEM;

	err = lower_inode->i_op->fiemap(lower_inode, fieinfo, start, len);
	sharefs_revert_fsids(saved_cred);

	return err;
}

/*
 * lsattr support.  FS_IOC_GETFLAGS never reaches
 * f_op->unlocked_ioctl (the VFS routes it through fileattr_get),
 * so sharefs must provide this entry or lsattr fails with ENOTTY.
 */
static int sharefs_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	struct path lower_path;
	const struct cred *saved_cred;
	int err;

	sharefs_get_lower_path(dentry, &lower_path);
	if (!lower_path.dentry || !d_inode(lower_path.dentry) ||
	    !d_inode(lower_path.dentry)->i_op ||
	    !d_inode(lower_path.dentry)->i_op->fileattr_get) {
		err = -EOPNOTSUPP;
		goto out;
	}

	saved_cred = sharefs_override_file_fsids(d_inode(dentry), NULL);
	if (!saved_cred) {
		err = -ENOMEM;
		goto out;
	}

	err = vfs_fileattr_get(lower_path.dentry, fa);
	sharefs_revert_fsids(saved_cred);

	sharefs_info_ratelimited("fileattr_get, err %d", err);
out:
	sharefs_put_lower_path(dentry, &lower_path);
	return err;
}

/*
 * chattr support -- pass fileattr through to the lower fs,
 * gated by the caller-ownership check below.
 */
static int sharefs_fileattr_set(struct mnt_idmap *idmap,
				struct dentry *dentry, struct fileattr *fa)
{
	struct inode *lower_inode;
	struct path lower_path;
	const struct cred *saved_cred;
	int err;

	/*
	 * Must run before the fsuid override below, which would let any
	 * caller pass the lower fs's own ownership check.
	 */
	if (!inode_owner_or_capable(idmap, d_inode(dentry))) {
		sharefs_err_ratelimited("fileattr_set denied, caller not owner");
		return -EACCES;
	}

	sharefs_get_lower_path(dentry, &lower_path);
	lower_inode = lower_path.dentry ? d_inode(lower_path.dentry) : NULL;
	if (!lower_inode || !lower_inode->i_op ||
	    !lower_inode->i_op->fileattr_set) {
		err = -EOPNOTSUPP;
		goto out;
	}

	saved_cred = sharefs_override_file_fsids(d_inode(dentry), NULL);
	if (!saved_cred) {
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Lower mount's idmap, not the upper one: the ownership check
	 * in vfs_fileattr_set() is against the lower inode, which is
	 * what overlayfs does too (ovl_real_fileattr_set()).  Degrades
	 * to &nop_mnt_idmap when the lower mount is not idmapped.
	 */
	err = vfs_fileattr_set(mnt_idmap(lower_path.mnt), lower_path.dentry,
			       fa);
	sharefs_revert_fsids(saved_cred);
	if (err)
		goto out;

	fsstack_copy_attr_all(d_inode(dentry), lower_inode);
	sharefs_info_ratelimited("fileattr_set, err %d", err);
out:
	sharefs_put_lower_path(dentry, &lower_path);
	return err;
}

const struct inode_operations sharefs_symlink_iops = {
	.permission	= sharefs_permission,
	.getattr	= sharefs_getattr,
	.get_link	= sharefs_get_link,
	.listxattr	= sharefs_listxattr,
	.fiemap		= sharefs_fiemap,
};

const struct inode_operations sharefs_dir_iops = {
	.lookup		= sharefs_lookup,
	.permission	= sharefs_permission,
	.getattr	= sharefs_getattr,
	.listxattr	= sharefs_listxattr,
	.setattr	= sharefs_setattr,
	.fileattr_get	= sharefs_fileattr_get,
	.fileattr_set	= sharefs_fileattr_set,
};

const struct inode_operations sharefs_dir_override_iops = {
	.lookup		= sharefs_override_lookup,
	.permission	= sharefs_override_permission,
	.getattr	= sharefs_getattr,
	.listxattr	= sharefs_listxattr,
	.setattr	= sharefs_setattr,
	.unlink		= sharefs_unlink,
	.rmdir		= sharefs_rmdir,
	.rename		= sharefs_rename,
	.create		= sharefs_create,
	.symlink	= sharefs_symlink,
	.mkdir		= sharefs_mkdir,
	.fileattr_get	= sharefs_fileattr_get,
	.fileattr_set	= sharefs_fileattr_set,
};

const struct inode_operations sharefs_main_iops = {
	.permission	= sharefs_permission,
	.getattr	= sharefs_getattr,
	.listxattr	= sharefs_listxattr,
	.setattr        = sharefs_setattr,
	.fiemap		= sharefs_fiemap,
	.fileattr_get	= sharefs_fileattr_get,
	.fileattr_set	= sharefs_fileattr_set,
};
