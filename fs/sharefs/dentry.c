// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/sharefs/dentry.c
 *
 * Copyright (c) 1998-2022 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2022 Stony Brook University
 * Copyright (c) 2003-2022 The Research Foundation of SUNY
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#include "sharefs.h"


static bool is_mountpoint(struct dentry *dentry)
{
	if (!dentry || !dentry->d_sb)
		return false;
	
	return d_mountpoint(dentry);
}

/*
 * returns: 0: tell VFS to invalidate dentry in share directory
 */
static int sharefs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	if (is_mountpoint(dentry)) {
		return 1;
	}

	return 0;
}

static void sharefs_d_release(struct dentry *dentry)
{
	/*
	 * It is possible that the dentry private data is NULL in case we
	 * ran out of memory while initializing it in
	 * new_dentry_private_data.  So check for NULL before attempting to
	 * release resources.
	 */
	if (SHAREFS_D(dentry)) {
		/* release and reset the lower paths */
		sharefs_put_reset_lower_path(dentry);
		free_dentry_private_data(dentry);
	}
	return;
}

/* standard case-sensitive dentry operations */
const struct dentry_operations sharefs_dops = {
	.d_revalidate	= sharefs_d_revalidate,
	.d_release	= sharefs_d_release,
};

/* case-insensitive dentry operations (ASCII only) */
/*
 * same semantics as sharefs_d_revalidate (always revalidate,
 * except mountpoints).  Returning 0 for every non-mountpoint dentry
 * (including the REVALIDATE_FOR_CASEFOLD_RENAME probe) forces the
 * casefold rename lookups onto the fresh-lookup path as well.
 */
static int sharefs_d_revalidate_nocase(struct dentry *dentry, unsigned int flags)
{
	if (is_mountpoint(dentry))
		return 1;

	return 0;
}

/*
 * hash the exact name so case variants such as A.txt and a.txt
 * live in different hash buckets and can coexist in the dcache.  A
 * case-folding hash would make d_lookup find only the first-created
 * variant and hide the other one.
 */
static int sharefs_d_hash_nocase(const struct dentry *dir, struct qstr *qstr)
{
	qstr->hash = full_name_hash(dir, qstr->name, qstr->len);
	return 0;
}

/*
 * ASCII case-insensitive compare.  Because d_hash does not fold,
 * variants never share a hash bucket and d_lookup will not get here;
 * real CI hits come from the directory scan fallback in
 * __sharefs_lookup.  Kept so hash and compare stay consistent.
 */
static int sharefs_d_compare_nocase(const struct dentry *dentry,
				    unsigned int len, const char *str,
				    const struct qstr *name)
{
	struct qstr q = QSTR_INIT(str, len);

	if (!qstr_case_eq(&q, name))
		return 1;
	return 0;
}

const struct dentry_operations sharefs_ci_dops = {
	.d_revalidate	= sharefs_d_revalidate_nocase,
	.d_release	= sharefs_d_release,
	.d_hash		= sharefs_d_hash_nocase,
	.d_compare	= sharefs_d_compare_nocase,
};

/*
 * pick dentry operations by mount mode.  Case-sensitive mounts
 * and CI-native lowers use the standard ops; sharefs adds the CI
 * fallback only when the lower fs is case-sensitive but the mount is
 * not.
 */
const struct dentry_operations *sharefs_pick_dops(struct super_block *sb)
{
	struct sharefs_sb_info *sbi = SHAREFS_SB(sb);

	if (sbi->case_sensitive)
		return &sharefs_dops;
	if (!sbi->lower_sensitive)	/* lower fs is already case-insensitive */
		return &sharefs_dops;
	return &sharefs_ci_dops;
}
