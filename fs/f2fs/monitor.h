/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _F2FS_MONITOR_H
#define _F2FS_MONITOR_H

#include <linux/fs.h>

/*
 * For files using f2fs_file_operations, file->private_data is reserved for
 * monitoring: NULL or an owned struct monitor_config reference for pending
 * content changes. f2fs_monitor_close() clears it and drops the reference.
 * Other features/backports must coordinate ownership, e.g. through a shared
 * per-file context, rather than overwrite this field independently.
 */

struct f2fs_sb_info;

enum f2fs_event_type {
	F2FS_F_CREATE,
	F2FS_F_MODIFY,
	F2FS_F_DELETE,
	F2FS_F_RENAME,
	F2FS_D_CREATE,
	F2FS_D_MODIFY,
	F2FS_D_DELETE,
	F2FS_D_RENAME,
};

struct f2fs_log_entry {
	__u8 event_type;
	__u8 metadata_only;
	__u16 path_len;
	__u16 old_path_len;
	__u8 data[];
} __packed;

int f2fs_monitor_init(struct f2fs_sb_info *sbi);
void f2fs_monitor_destroy(struct f2fs_sb_info *sbi);
void f2fs_monitor_inherit(struct inode *dir, struct inode *inode);
void f2fs_monitor_create(struct dentry *dentry, struct inode *inode);
void f2fs_monitor_event(struct dentry *dentry, enum f2fs_event_type type,
			bool metadata_only);
void f2fs_monitor_written(struct file *file);
void f2fs_monitor_close(struct file *file);
int f2fs_monitor_set(struct file *file, unsigned long arg);
int f2fs_monitor_get(struct file *file, unsigned long arg);
int f2fs_monitor_get_error(struct file *file, unsigned long arg);
int f2fs_monitor_prepare_rename(struct dentry *old, struct dentry *new,
		unsigned int flags, void **result);
void f2fs_monitor_finish_rename(void *context, int err);

#endif
