// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/sharefs/lookup.c
 *
 * Copyright (c) 1998-2022 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2022 Stony Brook University
 * Copyright (c) 2003-2022 The Research Foundation of SUNY
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#include "sharefs.h"
#include "authentication.h"

/* The dentry cache is just so we have properly sized dentries */
static struct kmem_cache *sharefs_dentry_cachep;

int sharefs_init_dentry_cache(void)
{
	sharefs_dentry_cachep =
		kmem_cache_create("sharefs_dentry",
				  sizeof(struct sharefs_dentry_info),
				  0, SLAB_RECLAIM_ACCOUNT, NULL);

	return sharefs_dentry_cachep ? 0 : -ENOMEM;
}

void sharefs_destroy_dentry_cache(void)
{
	if (sharefs_dentry_cachep)
		kmem_cache_destroy(sharefs_dentry_cachep);
}

void free_dentry_private_data(struct dentry *dentry)
{
	if (!dentry || !dentry->d_fsdata)
		return;
	kmem_cache_free(sharefs_dentry_cachep, dentry->d_fsdata);
	dentry->d_fsdata = NULL;
}

/* allocate new dentry private data */
int new_dentry_private_data(struct dentry *dentry)
{
	struct sharefs_dentry_info *info = SHAREFS_D(dentry);

	/* use zalloc to init dentry_info.lower_path */
	info = kmem_cache_zalloc(sharefs_dentry_cachep, GFP_ATOMIC);
	if (!info)
		return -ENOMEM;

	spin_lock_init(&info->lock);
	dentry->d_fsdata = info;

	return 0;
}

static int sharefs_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = sharefs_lower_inode(inode);
	if (current_lower_inode == (struct inode *)candidate_lower_inode)
		return 1; /* found a match */
	else
		return 0; /* no match */
}

static int sharefs_inode_set(struct inode *inode, void *lower_inode)
{
	/* we do actual inode initialization in sharefs_iget */
	return 0;
}

struct inode *sharefs_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct inode *inode; /* the new inode to return */

	if (!igrab(lower_inode))
		return ERR_PTR(-ESTALE);
	inode = iget5_locked(sb, /* our superblock */
			     /*
			      * hashval: we use inode number, but we can
			      * also use "(unsigned long)lower_inode"
			      * instead.
			      */
			     lower_inode->i_ino, /* hashval */
			     sharefs_inode_test, /* inode comparison function */
			     sharefs_inode_set, /* inode init function */
			     lower_inode); /* data passed to test+set fxns */
	if (!inode) {
		iput(lower_inode);
		return ERR_PTR(-ENOMEM);
	}

	if (lower_inode->i_nlink == 0) {
		iput(lower_inode);
		iput(inode);
		return ERR_PTR(-ENOENT);
	}

	/* if found a cached inode, then just return it (after iput) */
	if (!(inode->i_state & I_NEW)) {
		iput(lower_inode);
		return inode;
	}

	/* initialize new inode */
	inode->i_ino = lower_inode->i_ino;
	sharefs_set_lower_inode(inode, lower_inode);

	atomic64_inc(&inode->i_version);

	struct sharefs_sb_info *sbi = SHAREFS_SB(sb);

	/* use different set of inode ops for symlinks & directories */
	if (sbi && S_ISDIR(lower_inode->i_mode))
		if (sbi->override_support_delete) {
			inode->i_op = &sharefs_dir_override_iops;
		} else {
			inode->i_op = &sharefs_dir_iops;
		}
	else if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &sharefs_symlink_iops;
	else
		inode->i_op = &sharefs_main_iops;

	/* use different set of file ops for directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &sharefs_dir_fops;
	else
		inode->i_fop = &sharefs_main_fops;

	inode->i_atime.tv_sec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->__i_ctime.tv_sec = 0;
	inode->__i_ctime.tv_nsec = 0;

	/* properly initialize special inodes */
	if (S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
	    S_ISFIFO(lower_inode->i_mode) || S_ISSOCK(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
				   lower_inode->i_rdev);

	/* all well, copy inode attributes */
	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);

	unlock_new_inode(inode);
	return inode;
}

/*
 * Helper interpose routine, called directly by ->lookup to handle
 * spliced dentries.
 */
static struct dentry *__sharefs_interpose(struct dentry *dentry,
					 struct super_block *sb,
					 struct path *lower_path)
{
	struct inode *inode;
	struct inode *lower_inode = d_inode(lower_path->dentry);
	struct dentry *ret_dentry;

	/*
	 * We allocate our new inode below by calling sharefs_iget,
	 * which will initialize some of the new inode's fields
	 */

	/* inherit lower inode number for sharefs's inode */
	inode = sharefs_iget(sb, lower_inode);
	if (IS_ERR(inode)) {
		ret_dentry = ERR_PTR(PTR_ERR(inode));
		goto out;
	}

	ret_dentry = d_splice_alias(inode, dentry);

out:
	return ret_dentry;
}

/*
 * Connect a sharefs inode dentry/inode with several lower ones.  This is
 * the classic stackable file system "vnode interposition" action.
 *
 * @dentry: sharefs's dentry which interposes on lower one
 * @sb: sharefs's super_block
 * @lower_path: the lower path (caller does path_get/put)
 */
int sharefs_interpose(struct dentry *dentry, struct super_block *sb,
		     struct path *lower_path)
{
	struct dentry *ret_dentry;

	ret_dentry = __sharefs_interpose(dentry, sb, lower_path);
	return PTR_ERR(ret_dentry);
}

/* lower case-sensitivity probe and CI fallback scan */

/*
 * probe whether the lower fs is case-sensitive.  Lowers with a
 * casefold encoding or with a d_compare in s_d_op (vfat, exfat) are
 * case-insensitive; native Linux file systems (ext4, f2fs, tmpfs, ...)
 * are case-sensitive.
 */
bool sharefs_lower_is_case_sensitive(struct super_block *lower_sb)
{
	if (sb_has_encoding(lower_sb))
		return false;
	if (lower_sb->s_d_op && lower_sb->s_d_op->d_compare)
		return false;
	return true;
}

/*
 * only names containing ASCII letters can have case variants;
 * everything else skips the directory scan entirely.
 */
bool sharefs_name_needs_ci(const struct qstr *name)
{
	int i;

	for (i = 0; i < name->len; i++) {
		unsigned char c = name->name[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
			return true;
	}
	return false;
}

/* iterate_dir scan context: target name plus hit result */
struct sharefs_ci_scan_ctx {
	struct dir_context ctx;
	const struct qstr *target;
	char *hit;
	size_t hit_len;
	bool found;
	bool oom;
};

/*
 * readdir callback comparing each entry with the target
 * case-insensitively; a hit remembers the actual on-disk name.
 * filldir_t semantics: return true to continue iteration and false to
 * stop, so return false on a hit or on OOM.
 */
static bool sharefs_ci_scan_actor(struct dir_context *ctx, const char *name,
				 int namlen, loff_t offset, u64 ino,
				 unsigned int d_type)
{
	struct sharefs_ci_scan_ctx *s =
		container_of(ctx, struct sharefs_ci_scan_ctx, ctx);
	struct qstr entry = QSTR_INIT(name, namlen);

	if (s->found)
		return false;
	if (!qstr_case_eq(&entry, s->target))
		return true;	/* no match, keep going */
	/* GFP_NOFS: called under the lower dir's shared inode lock */
	s->hit = kmemdup(name, namlen, GFP_NOFS);
	if (!s->hit) {
		s->oom = true;
		return false;	/* OOM: stop the scan */
	}
	s->hit_len = namlen;
	s->found = true;
	return false;	/* hit: stop */
}

/*
 * ASCII case-insensitive scan of the lower parent directory,
 * used as a fallback after an exact lookup returns -ENOENT.  Returns
 * the matched lower dentry (caller dputs), NULL if no variant exists,
 * or an ERR_PTR on error.  Callers must not hold the directory inode's
 * exclusive lock: iterate_dir takes a shared lock on it.
 */
struct dentry *sharefs_lower_ci_lookup(struct super_block *sb, struct inode *dir,
				       const struct qstr *name,
				       struct dentry *lower_dir_dentry,
				       struct vfsmount *lower_dir_mnt)
{
	struct sharefs_ci_scan_ctx scan;
	struct path dir_path;
	struct file *dir_file;
	struct dentry *res = NULL;
	const struct cred *saved_cred = NULL;
	int err = 0;

	memset(&scan, 0, sizeof(scan));
	scan.ctx.actor = sharefs_ci_scan_actor;
	scan.ctx.pos = 0;
	scan.target = name;

	/*
	 * lower_dir_dentry is borrowed and path_put(&dir_path) below
	 * drops it, so take our own reference first.
	 */
	dir_path.dentry = dget(lower_dir_dentry);
	dir_path.mnt = mntget(lower_dir_mnt);

	/* open the lower directory with mount credentials */
	saved_cred = sharefs_override_file_fsids(dir, NULL);
	if (!saved_cred) {
		path_put(&dir_path);
		return ERR_PTR(-ENOMEM);
	}

	dir_file = dentry_open(&dir_path, O_RDONLY | O_DIRECTORY, current_cred());
	path_put(&dir_path);
	if (IS_ERR(dir_file)) {
		err = PTR_ERR(dir_file);
		goto out;
	}

	err = iterate_dir(dir_file, &scan.ctx);
	fput(dir_file);
	if (err)
		goto out;
	if (scan.oom) {
		err = -ENOMEM;
		goto out;
	}

	if (scan.found) {
		/*
		 * Exact lookup with the on-disk name found by the scan.
		 * Invalidate a stale negative and retry once; a still
		 * negative result means not found (never interpose a
		 * negative dentry).
		 */
		res = lookup_one_len_unlocked(scan.hit, lower_dir_dentry,
					      scan.hit_len);
		if (!IS_ERR(res) && d_is_negative(res)) {
			d_invalidate(res);
			dput(res);
			res = lookup_one_len_unlocked(scan.hit, lower_dir_dentry,
						      scan.hit_len);
		}
		if (!IS_ERR(res) && d_is_negative(res)) {
			dput(res);
			res = NULL;
		}
		if (IS_ERR(res))
			err = PTR_ERR(res);
	}
out:
	kfree(scan.hit);
	sharefs_revert_fsids(saved_cred);
	if (err)
		return ERR_PTR(err);
	return res;
}

/*
 * Main driver function for sharefs's lookup.
 *
 * Returns: NULL (ok), ERR_PTR if an error occurred.
 * Fills in lower_parent_path with <dentry,mnt> on success.
 */
static struct dentry *__sharefs_lookup(struct dentry *dentry,
				      unsigned int flags,
				      struct path *lower_parent_path)
{
	int err = 0;
	struct vfsmount *lower_dir_mnt;
	struct dentry *lower_dir_dentry = NULL;
	struct dentry *lower_dentry;
	const char *name;
	struct path lower_path;
	struct dentry *ret_dentry = NULL;

	/* must initialize dentry operations */
	d_set_d_op(dentry, sharefs_pick_dops(dentry->d_sb));

	if (IS_ROOT(dentry))
		goto out;

	name = dentry->d_name.name;

	/* now start the actual lookup procedure */
	lower_dir_dentry = lower_parent_path->dentry;
	lower_dir_mnt = lower_parent_path->mnt;

	/* Use vfs_path_lookup to check if the dentry exists or not */
	err = vfs_path_lookup(lower_dir_dentry, lower_dir_mnt, name, 0,
			      &lower_path);
	/* no error: handle positive dentries */
	if (!err) {
		sharefs_set_lower_path(dentry, &lower_path);
		ret_dentry =
			__sharefs_interpose(dentry, dentry->d_sb, &lower_path);
		if (IS_ERR(ret_dentry)) {
			err = PTR_ERR(ret_dentry);
			 /* path_put underlying path on error */
			sharefs_put_reset_lower_path(dentry);
		}
		goto out;
	}
	/*
	 * We don't consider ENOENT an error, and we want to return a
	 * negative dentry.
	 */
	if (err && err != -ENOENT)
		goto out;

	/*
	 * exact lookup failed -- scan the lower parent for a case
	 * variant before settling on a negative dentry.  Create/rename
	 * targets are excluded because they must stay negative (the VFS
	 * same-inode check would swallow the rename), and the
	 * casefold-rename probe because that path already holds the
	 * parent lock iterate_dir() would need.
	 */
	if (!SHAREFS_SB(dentry->d_sb)->case_sensitive &&
	    SHAREFS_SB(dentry->d_sb)->lower_sensitive &&
	    sharefs_name_needs_ci(&dentry->d_name) &&
	    !(flags & (LOOKUP_CREATE | LOOKUP_RENAME_TARGET |
		       REVALIDATE_FOR_CASEFOLD_RENAME))) {
		struct dentry *ci;

		ci = sharefs_lower_ci_lookup(dentry->d_sb,
					     d_backing_inode(dentry->d_parent),
					     &dentry->d_name, lower_dir_dentry,
					     lower_dir_mnt);
		if (IS_ERR(ci)) {
			sharefs_warning_ratelimited("ci lookup err %ld",
						    PTR_ERR(ci));
		} else if (ci) {
			struct dentry *neg;

			sharefs_info_ratelimited("ci lookup hit (case-variant matched)");
			/*
			 * The failed exact lookup left a negative dentry in the
			 * lower dcache that would shadow the variant we just
			 * matched -- a direct walk of the lower would report
			 * ENOENT for a file that exists.  Drop it.
			 */
			neg = lookup_one_len_unlocked(name, lower_dir_dentry,
						      dentry->d_name.len);
			if (!IS_ERR(neg)) {
				if (d_is_negative(neg))
					d_invalidate(neg);
				dput(neg);
			}
			lower_path.dentry = ci;
			lower_path.mnt = mntget(lower_dir_mnt);
			sharefs_set_lower_path(dentry, &lower_path);
			ret_dentry = __sharefs_interpose(dentry, dentry->d_sb,
							 &lower_path);
			if (IS_ERR(ret_dentry)) {
				err = PTR_ERR(ret_dentry);
				sharefs_put_reset_lower_path(dentry);
			} else {
				err = 0;
			}
			goto out;
		}
		/* no variant in the lower fs: fall through to the negative path */
	}

	lower_dentry = lookup_one_len_unlocked(name, lower_dir_dentry, dentry->d_name.len);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		sharefs_err("lookup_one_len_unlocked error:%d", err);
		ret_dentry = lower_dentry;
		goto out;
	}

	/*
	 * the rename target stays negative on the upper side on
	 * purpose, so the scan above skipped it; but the lower dentry
	 * just hashed is not consumed by the casefold rename path and
	 * would survive as a stale negative shadowing the name the
	 * rename created.  Unhash the lower one only.
	 */
	if (d_is_negative(lower_dentry) &&
	    !SHAREFS_SB(dentry->d_sb)->case_sensitive &&
	    SHAREFS_SB(dentry->d_sb)->lower_sensitive &&
	    sharefs_name_needs_ci(&dentry->d_name) &&
	    (flags & LOOKUP_RENAME_TARGET))
		d_invalidate(lower_dentry);

	lower_path.dentry = lower_dentry;
	lower_path.mnt = mntget(lower_dir_mnt);
	sharefs_set_lower_path(dentry, &lower_path);

	/*
	 * If the intent is to create a file, then don't return an error, so
	 * the VFS will continue the process of making this negative dentry
	 * into a positive one.
	 */
	if (err == -ENOENT || (flags & (LOOKUP_CREATE|LOOKUP_RENAME_TARGET)))
		err = 0;

out:
	if (err)
		return ERR_PTR(err);
	return ret_dentry;
}

struct dentry *sharefs_lookup(struct inode *dir, struct dentry *dentry,
			     unsigned int flags)
{
	int err;
	struct dentry *ret, *parent;
	struct path lower_parent_path;
	parent = dget_parent(dentry);
	sharefs_get_lower_path(parent, &lower_parent_path);

	/* allocate dentry private data.  We free it in ->d_release */
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out;
	}
	ret = __sharefs_lookup(dentry, flags, &lower_parent_path);
	if (IS_ERR(ret)) {
		sharefs_err("sharefs_lookup error!");
		goto out;
	}

	if (ret)
		dentry = ret;
	if (d_inode(dentry))
		fsstack_copy_attr_times(d_inode(dentry),
					sharefs_lower_inode(d_inode(dentry)));
	/* update parent directory's atime */
	fsstack_copy_attr_atime(d_inode(parent),
				sharefs_lower_inode(d_inode(parent)));
	fixup_perm_from_level(d_inode(parent), dentry);
out:
	sharefs_put_lower_path(parent, &lower_parent_path);
	dput(parent);
	return ret;
}

struct dentry *sharefs_override_lookup(struct inode *dir, struct dentry *dentry,
			     unsigned int flags)
{
	int err;
	struct dentry *ret, *parent;
	struct path lower_parent_path;
	const struct cred *saved_cred = NULL;
	__u16 permission;
	__u16 next_permission;

	parent = dget_parent(dentry);
	sharefs_get_lower_path(parent, &lower_parent_path);
	
	next_permission = perm_get_next_level_override(SHAREFS_I(dir)->perm);
	if (next_permission == SHAREFS_PERM_PACKAGE_OVERRIDE && SHAREFS_SB(dir->i_sb)->sniffer) {
		printk("sharefs: package override enable\n");
	}

	saved_cred = sharefs_override_file_fsids(dir, &permission);
	if (!saved_cred) {
		ret = ERR_PTR(-ENOMEM);
		goto out_err;
	}

	/* allocate dentry private data.  We free it in ->d_release */
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out;
	}
	ret = __sharefs_lookup(dentry, flags, &lower_parent_path);
	if (IS_ERR(ret)) {
		sharefs_err("sharefs_lookup error!");
		goto out;
	}

	if (ret)
		dentry = ret;
	if (d_inode(dentry)) {
		fsstack_copy_attr_times(d_inode(dentry),
					sharefs_lower_inode(d_inode(dentry)));
		SHAREFS_I(d_inode(dentry))->perm = next_permission;
	}
	/* update parent directory's atime */
	fsstack_copy_attr_atime(d_inode(parent),
				sharefs_lower_inode(d_inode(parent)));
out:
	sharefs_revert_fsids(saved_cred);
out_err:
	sharefs_put_lower_path(parent, &lower_parent_path);
	dput(parent);
	return ret;
}