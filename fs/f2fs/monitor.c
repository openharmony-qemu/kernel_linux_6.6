// SPDX-License-Identifier: GPL-2.0
#include <linux/f2fs_fs.h>
#include <linux/hashtable.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/refcount.h>

#include "f2fs.h"
#include "monitor.h"

#define MONITOR_LOG_DIR "/data/log"
#define MONITOR_LOG_NAME_LEN 32
#define MONITOR_QUEUE_LIMIT (4 * 1024 * 1024)

/*
 * A config pins root until its last put. sbi is borrowed: filesystem teardown
 * drains monitor work and releases configs before freeing sbi.
 */
struct monitor_config {
	struct hlist_node node;
	refcount_t refs;
	struct f2fs_sb_info *sbi;
	struct inode *root;
	unsigned long root_ino;
	const struct cred *cred;
	int error;
};

struct monitor_record {
	struct list_head list;
	struct monitor_config *config;
	size_t size;
	bool remove;
	u8 data[];
};

struct f2fs_monitor {
	/* Protect roots and serialize records through VFS rename completion. */
	struct rw_semaphore sem;
	struct mutex config_lock;
	DECLARE_HASHTABLE(roots, 8);
	spinlock_t queue_lock;
	struct list_head pending;
	size_t queued_bytes;
	struct work_struct work;
	/*
	 * Synchronous log-I/O recursion guard, not an async I/O origin marker.
	 * Writeback/compression completion paths do not emit monitor events.
	 */
	struct task_struct *writer;
};

struct monitor_inode {
	struct list_head list;
	struct inode *inode;
};

struct f2fs_monitor_rename {
	struct f2fs_monitor *monitor;
	struct list_head inodes;
	struct list_head records;
	bool monitored;
};

static void monitor_get(struct monitor_config *config)
{
	refcount_inc(&config->refs);
}

static void monitor_put(struct monitor_config *config)
{
	if (refcount_dec_and_test(&config->refs)) {
		iput(config->root);
		put_cred(config->cred);
		kfree(config);
	}
}

/*
 * Latch the first failure: even a transient error can leave missing or partial
 * records. Clearing error cannot repair the journal. Recovery requires a new
 * backup baseline and disabling/re-enabling monitoring on a quiescent subtree.
 * Caller holds a config reference or protects the table's reference.
 */
static void monitor_error(struct monitor_config *config, int err)
{
	if (err && !cmpxchg(&config->error, 0, err))
		f2fs_err(config->sbi,
			 "monitor %lu: log incomplete (error %d); a new backup is required",
			 config->root_ino, err);
}

/* Table updates hold config_lock and sem; lookups require either lock. */
static struct monitor_config *monitor_lookup(struct f2fs_sb_info *sbi,
					    unsigned long ino)
{
	struct monitor_config *config;

	hash_for_each_possible(sbi->monitor->roots, config, node, ino)
		if (config->root_ino == ino)
			return config;
	return NULL;
}

void f2fs_monitor_inherit(struct inode *dir, struct inode *inode)
{
	if ((!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode)) ||
	    is_inode_flag_set(inode, FI_MONITOR_ROOT))
		return;
	if (is_inode_flag_set(dir, FI_MONITORED))
		set_inode_flag(inode, FI_MONITORED);
	else
		clear_inode_flag(inode, FI_MONITORED);
}

/* rename_lock makes both ancestry and names stable while copying. No I/O. */
static struct monitor_config *monitor_root(struct dentry *entry)
{
	for (;;) {
		struct inode *inode = d_inode(entry);

		if (inode && is_inode_flag_set(inode, FI_MONITOR_ROOT))
			return monitor_lookup(F2FS_I_SB(inode), inode->i_ino);
		if (IS_ROOT(entry))
			return NULL;
		entry = entry->d_parent;
	}
}

static struct monitor_config *monitor_find_root(struct dentry *entry)
{
	struct monitor_config *config;

	read_seqlock_excl(&rename_lock);
	config = monitor_root(entry);
	read_sequnlock_excl(&rename_lock);
	return config;
}

/*
 * Build backwards; the returned length identifies the suffix at buffer's end.
 * Caller holds rename_lock across both paths of a rename record.
 */
static int monitor_path(struct dentry *entry, struct monitor_config *config,
			char *buffer, size_t size)
{
	char *end = buffer + size, *pos = end;
	int err = -ENOENT;

	for (;;) {
		struct inode *inode = d_inode(entry);
		size_t len;

		if (inode == config->root) {
			err = end - pos;
			break;
		}
		if (IS_ROOT(entry))
			break;
		len = entry->d_name.len;
		if (len + (pos != end) > pos - buffer) {
			err = -ENAMETOOLONG;
			break;
		}
		if (pos != end)
			*--pos = '/';
		pos -= len;
		memcpy(pos, entry->d_name.name, len);
		entry = entry->d_parent;
	}
	return err;
}

static struct monitor_record *monitor_record(struct monitor_config *config,
		enum f2fs_event_type type, bool metadata_only,
		struct dentry *entry, struct dentry *old)
{
	struct monitor_record *record = NULL;
	struct f2fs_log_entry *log;
	char *buffer;
	size_t size = PATH_MAX;
	int len, old_len = 0;

	if (READ_ONCE(config->error))
		return NULL;
retry:
	buffer = kvmalloc(size * (old ? 2 : 1), GFP_NOFS);
	if (!buffer) {
		monitor_error(config, -ENOMEM);
		return NULL;
	}
	read_seqlock_excl(&rename_lock);
	len = monitor_path(entry, config, buffer, size);
	if (len >= 0 && old)
		old_len = monitor_path(old, config, buffer + size, size);
	read_sequnlock_excl(&rename_lock);
	if ((len == -ENAMETOOLONG || old_len == -ENAMETOOLONG) &&
	    size < U16_MAX) {
		kvfree(buffer);
		size = U16_MAX;
		goto retry;
	}
	if (len < 0 || old_len < 0) {
		monitor_error(config, len < 0 ? len : old_len);
		goto out;
	}
	record = kmalloc(sizeof(*record) + sizeof(*log) + len + old_len,
			 GFP_NOFS);
	if (!record) {
		monitor_error(config, -ENOMEM);
		goto out;
	}
	monitor_get(config);
	record->config = config;
	record->remove = false;
	record->size = sizeof(*log) + len + old_len;
	log = (struct f2fs_log_entry *)record->data;
	log->event_type = type;
	log->metadata_only = metadata_only;
	log->path_len = len;
	log->old_path_len = old_len;
	memcpy(log->data, buffer + size - len, len);
	if (old)
		memcpy(log->data + len, buffer + 2 * size - old_len, old_len);
out:
	kvfree(buffer);
	return record;
}

static void monitor_free_record(struct monitor_record *record)
{
	monitor_put(record->config);
	kfree(record);
}

static void monitor_queue(struct monitor_record *record)
{
	struct f2fs_monitor *monitor;
	bool full;

	if (!record)
		return;
	monitor = record->config->sbi->monitor;
	/*
	 * Cancellation holds config_lock until flush_work() completes, so at
	 * most one removal record can be pending or running per filesystem.
	 * Let it bypass the limit so a full queue cannot prevent log cleanup;
	 * its header is still charged to queued_bytes below (size is zero).
	 */
	spin_lock(&monitor->queue_lock);
	full = !record->remove && monitor->queued_bytes + sizeof(*record) +
			record->size > MONITOR_QUEUE_LIMIT;
	if (!full) {
		monitor->queued_bytes += sizeof(*record) + record->size;
		list_add_tail(&record->list, &monitor->pending);
	}
	spin_unlock(&monitor->queue_lock);
	if (full) {
		monitor_error(record->config, -ENOSPC);
		monitor_free_record(record);
	} else {
		queue_work(system_unbound_wq, &monitor->work);
	}
}

void f2fs_monitor_event(struct dentry *dentry, enum f2fs_event_type type,
			bool metadata_only)
{
	struct inode *inode = d_inode(dentry);
	struct f2fs_monitor *monitor;
	struct monitor_config *config;

	if (!inode || (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode)) ||
	    !is_inode_flag_set(inode, FI_MONITORED) ||
	    current == READ_ONCE(F2FS_I_SB(inode)->monitor->writer))
		return;
	if (!inode->i_nlink && (type == F2FS_F_MODIFY || type == F2FS_D_MODIFY))
		return;
	monitor = F2FS_I_SB(inode)->monitor;
	down_read(&monitor->sem);
	config = monitor_find_root(dentry);
	if (config)
		monitor_queue(monitor_record(config, type, metadata_only,
					    dentry, NULL));
	up_read(&monitor->sem);
}

/* Queue before d_instantiate_new() exposes a directory to child creators. */
void f2fs_monitor_create(struct dentry *dentry, struct inode *inode)
{
	struct f2fs_monitor *monitor;
	struct monitor_config *config;

	if (!is_inode_flag_set(inode, FI_MONITORED))
		return;
	monitor = F2FS_I_SB(inode)->monitor;
	down_read(&monitor->sem);
	config = monitor_find_root(dentry->d_parent);
	if (config)
		monitor_queue(monitor_record(config, S_ISDIR(inode->i_mode) ?
					    F2FS_D_CREATE : F2FS_F_CREATE,
					    false, dentry, NULL));
	up_read(&monitor->sem);
}

/* A config reference in file->private_data marks pending content changes. */
void f2fs_monitor_written(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct f2fs_monitor *monitor;
	struct monitor_config *config, *previous = file->private_data;

	if (!is_inode_flag_set(inode, FI_MONITORED) ||
	    current == READ_ONCE(F2FS_I_SB(inode)->monitor->writer))
		return;
	monitor = F2FS_I_SB(inode)->monitor;
	down_read(&monitor->sem);
	config = monitor_find_root(file->f_path.dentry);
	if (config && config != previous) {
		monitor_get(config);
		file->private_data = config;
	} else {
		previous = NULL;
	}
	up_read(&monitor->sem);
	if (previous)
		monitor_put(previous);
}

void f2fs_monitor_close(struct file *file)
{
	struct monitor_config *config = file->private_data;
	struct inode *inode = file_inode(file);
	struct f2fs_monitor *monitor;

	if (!config)
		return;
	file->private_data = NULL;
	if (!is_inode_flag_set(inode, FI_MONITORED))
		goto out;
	monitor = F2FS_I_SB(inode)->monitor;
	/* Match VFS rename/unlink: inode lock before the record's read lock. */
	inode_lock(inode);
	down_read(&monitor->sem);
	if (inode->i_nlink && monitor_find_root(file->f_path.dentry) == config)
		monitor_queue(monitor_record(config, F2FS_F_MODIFY, false,
					    file->f_path.dentry, NULL));
	up_read(&monitor->sem);
	inode_unlock(inode);
out:
	monitor_put(config);
}

static void monitor_log_name(struct monitor_config *config, char *name,
			     size_t size)
{
	snprintf(name, size, "%lu.log", config->root_ino);
}

static struct file *monitor_open_log(struct monitor_config *config, bool *created)
{
	struct path parent;
	struct file *file;
	char name[MONITOR_LOG_NAME_LEN];
	int err;

	err = kern_path(MONITOR_LOG_DIR, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
	if (err)
		return ERR_PTR(err);
	/* Keep the journal outside every monitored tree. */
	if ((parent.dentry->d_sb->s_magic == F2FS_SUPER_MAGIC &&
	     is_inode_flag_set(d_inode(parent.dentry), FI_MONITORED)) ||
	    !uid_eq(d_inode(parent.dentry)->i_uid, GLOBAL_ROOT_UID) ||
	    (d_inode(parent.dentry)->i_mode & (S_IWGRP | S_IWOTH))) {
		path_put(&parent);
		return ERR_PTR(-EPERM);
	}
	monitor_log_name(config, name, sizeof(name));
	file = file_open_root(&parent, name, O_WRONLY | O_APPEND | O_NOFOLLOW |
			     O_NONBLOCK | O_LARGEFILE |
			     (created ? O_CREAT | O_EXCL : 0), created ? 0644 : 0);
	if (created && IS_ERR(file) && PTR_ERR(file) == -EEXIST)
		file = file_open_root(&parent, name, O_WRONLY | O_APPEND |
				     O_NOFOLLOW | O_NONBLOCK | O_LARGEFILE, 0);
	else if (created && !IS_ERR(file))
		*created = true;
	path_put(&parent);
	if (IS_ERR(file))
		return file;
	if (!S_ISREG(file_inode(file)->i_mode) ||
	    file_inode(file)->i_nlink != 1 ||
	    !uid_eq(file_inode(file)->i_uid, GLOBAL_ROOT_UID) ||
	    (file_inode(file)->i_sb->s_magic == F2FS_SUPER_MAGIC &&
	     is_inode_flag_set(file_inode(file), FI_MONITORED))) {
		filp_close(file, NULL);
		return ERR_PTR(-EINVAL);
	}
	return file;
}

static int monitor_remove_log(struct monitor_config *config)
{
	struct path parent;
	struct dentry *entry;
	struct inode *inode = NULL;
	char name[MONITOR_LOG_NAME_LEN];
	int err;

	err = kern_path(MONITOR_LOG_DIR, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
	if (err)
		return err;
	if (!uid_eq(d_inode(parent.dentry)->i_uid, GLOBAL_ROOT_UID) ||
	    (d_inode(parent.dentry)->i_mode & (S_IWGRP | S_IWOTH))) {
		err = -EPERM;
		goto out;
	}
	err = mnt_want_write(parent.mnt);
	if (err)
		goto out;
	monitor_log_name(config, name, sizeof(name));
	inode_lock_nested(d_inode(parent.dentry), I_MUTEX_PARENT);
	entry = lookup_one(mnt_idmap(parent.mnt), name, parent.dentry, strlen(name));
	if (IS_ERR(entry)) {
		err = PTR_ERR(entry);
	} else {
		inode = d_inode(entry);
		if (inode) {
			/* Defer possible truncation until after unlocking the parent. */
			ihold(inode);
			if (!S_ISREG(inode->i_mode) ||
			    !uid_eq(inode->i_uid, GLOBAL_ROOT_UID))
				err = -EPERM;
			else
				err = vfs_unlink(mnt_idmap(parent.mnt),
						d_inode(parent.dentry), entry, NULL);
		}
		dput(entry);
	}
	inode_unlock(d_inode(parent.dentry));
	iput(inode);
	mnt_drop_write(parent.mnt);
out:
	path_put(&parent);
	return err;
}

static void monitor_work(struct work_struct *work)
{
	struct f2fs_monitor *monitor = container_of(work, struct f2fs_monitor, work);
	struct monitor_record *record;

	WRITE_ONCE(monitor->writer, current);
	for (;;) {
		struct monitor_config *config;
		const struct cred *saved;
		struct file *file;
		ssize_t ret = 0;

		spin_lock(&monitor->queue_lock);
		if (list_empty(&monitor->pending)) {
			spin_unlock(&monitor->queue_lock);
			break;
		}
		record = list_first_entry(&monitor->pending, struct monitor_record, list);
		list_del(&record->list);
		monitor->queued_bytes -= sizeof(*record) + record->size;
		spin_unlock(&monitor->queue_lock);
		config = record->config;
		saved = override_creds(config->cred);
		if (record->remove) {
			ret = monitor_remove_log(config);
		} else if (!READ_ONCE(config->error)) {
			file = monitor_open_log(config, NULL);
			if (IS_ERR(file)) {
				ret = PTR_ERR(file);
			} else {
				ret = kernel_write(file, record->data, record->size,
						   &file->f_pos);
				if (ret >= 0)
					ret = ret == record->size ? 0 : -EIO;
				filp_close(file, NULL);
			}
		}
		revert_creds(saved);
		monitor_error(config, ret);
		monitor_free_record(record);
		cond_resched();
	}
	WRITE_ONCE(monitor->writer, NULL);
}

static int monitor_add_inode(struct list_head *list, struct inode *inode,
			     struct xarray *seen)
{
	struct monitor_inode *item;
	int err;

	/* Bound malformed directory cycles; duplicate regular inodes need no work. */
	err = xa_insert(seen, inode->i_ino, xa_mk_value(1), GFP_NOFS);
	if (err) {
		iput(inode);
		return err == -EBUSY ? 0 : err;
	}
	item = kmalloc(sizeof(*item), GFP_NOFS);
	if (!item) {
		iput(inode);
		return -ENOMEM;
	}
	item->inode = inode;
	list_add_tail(&item->list, list);
	return 0;
}

static void monitor_free_inodes(struct list_head *list)
{
	struct monitor_inode *item, *next;

	list_for_each_entry_safe(item, next, list, list) {
		list_del(&item->list);
		iput(item->inode);
		kfree(item);
	}
}

static int monitor_collect_entries(struct f2fs_dentry_ptr *d,
				    struct list_head *list, struct xarray *seen)
{
	unsigned int bit = 0;

	while ((bit = find_next_bit_le(d->bitmap, d->max, bit)) < d->max) {
		struct f2fs_dir_entry *de = &d->dentry[bit];
		unsigned int len = le16_to_cpu(de->name_len);
		unsigned int slots = GET_DENTRY_SLOTS(len);
		struct inode *inode;
		int err;

		if (!len || len > F2FS_NAME_LEN || bit + slots > d->max)
			return -EFSCORRUPTED;
		if ((len == 1 && d->filename[bit][0] == '.') ||
		    (len == 2 && d->filename[bit][0] == '.' &&
				 d->filename[bit][1] == '.'))
			goto next;
		/* The backup interface supports directories and regular files only. */
		if (de->file_type != FT_DIR && de->file_type != FT_REG_FILE)
			goto next;
		inode = f2fs_iget(d->inode->i_sb, le32_to_cpu(de->ino));
		if (IS_ERR(inode))
			return PTR_ERR(inode);
		if (inode == d->inode || is_inode_flag_set(inode, FI_MONITOR_ROOT)) {
			iput(inode);
			return -EBUSY;
		}
		err = monitor_add_inode(list, inode, seen);
		if (err)
			return err;
next:
		bit += slots;
	}
	return 0;
}

/*
 * Userspace must quiesce this subtree during SET_MONITOR and directory renames
 * across a monitoring boundary. Ordinary namespace operations do not take sem.
 * Do not acquire i_rwsem: monitored hooks can hold it while waiting for sem.
 * Collect before changing any flags, so an allocation/read error is harmless.
 * The flat list also avoids recursion on the kernel stack.
 */
static int monitor_collect(struct inode *root, struct list_head *list)
{
	struct monitor_inode *item;
	DEFINE_XARRAY(seen);
	int err;

	ihold(root);
	err = monitor_add_inode(list, root, &seen);
	if (err)
		goto out;
	list_for_each_entry(item, list, list) {
		struct inode *dir = item->inode;
		struct f2fs_dentry_ptr d;
		struct page *page;
		pgoff_t n = 0, pages;

		if (!S_ISDIR(dir->i_mode))
			continue;
		if (f2fs_has_inline_dentry(dir)) {
			page = f2fs_get_node_page(F2FS_I_SB(dir), dir->i_ino);
			if (IS_ERR(page)) {
				err = PTR_ERR(page);
				goto out;
			}
			make_dentry_ptr_inline(dir, &d, inline_data_addr(dir, page));
			err = monitor_collect_entries(&d, list, &seen);
			f2fs_put_page(page, 1);
			if (err)
				goto out;
			continue;
		}
		pages = DIV_ROUND_UP(i_size_read(dir), PAGE_SIZE);
		while (n < pages) {
			pgoff_t next;

			page = f2fs_find_data_page(dir, n, &next);
			if (IS_ERR(page)) {
				if (PTR_ERR(page) != -ENOENT) {
					err = PTR_ERR(page);
					goto out;
				}
				n = next;
				continue;
			}
			make_dentry_ptr_block(dir, &d, page_address(page));
			err = monitor_collect_entries(&d, list, &seen);
			f2fs_put_page(page, 0);
			if (err)
				goto out;
			n++;
		}
		cond_resched();
	}
	err = 0;
out:
	xa_destroy(&seen);
	return err;
}

static void monitor_mark(struct list_head *list, bool enabled)
{
	struct monitor_inode *item;

	list_for_each_entry(item, list, list) {
		if (enabled)
			set_inode_flag(item->inode, FI_MONITORED);
		else
			clear_inode_flag(item->inode, FI_MONITORED);
	}
}

int f2fs_monitor_set(struct file *file, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_monitor *monitor = sbi->monitor;
	struct monitor_config *config, *cancelled = NULL;
	struct monitor_record *remove = NULL;
	struct file *log = NULL;
	LIST_HEAD(inodes);
	bool created = false;
	int err = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!S_ISDIR(inode->i_mode))
		return -EINVAL;
	mutex_lock(&monitor->config_lock);
	if (arg && unlikely(f2fs_cp_error(sbi))) {
		err = -EIO;
		goto out_mutex;
	}
	/* Userspace must stop operations on this subtree until the ioctl returns. */
	if (arg) {
		config = kzalloc(sizeof(*config), GFP_KERNEL);
		if (!config) {
			err = -ENOMEM;
			goto out_mutex;
		}
		refcount_set(&config->refs, 1);
		ihold(inode);
		config->sbi = sbi;
		config->root = inode;
		config->root_ino = inode->i_ino;
		config->cred = get_current_cred();
		log = monitor_open_log(config, &created);
		if (IS_ERR(log)) {
			err = PTR_ERR(log);
			log = NULL;
			goto out_put;
		}
	} else {
		remove = kzalloc(sizeof(*remove), GFP_KERNEL);
		if (!remove) {
			err = -ENOMEM;
			goto out_mutex;
		}
		config = NULL;
	}
	if (!inode->i_nlink || IS_DEADDIR(inode)) {
		err = -ENOENT;
		goto out_inodes;
	}
	if (arg) {
		if (is_inode_flag_set(inode, FI_MONITORED)) {
			err = -EEXIST;
			goto out_inodes;
		}
		if (log->f_path.dentry->d_sb == inode->i_sb &&
		    is_subdir(log->f_path.dentry, file->f_path.dentry)) {
			err = -EINVAL;
			goto out_inodes;
		}
	} else {
		config = monitor_lookup(sbi, inode->i_ino);
		if (!config) {
			err = -ENOENT;
			goto out_inodes;
		}
	}
	err = monitor_collect(inode, &inodes);
	if (err)
		goto out_inodes;
	/* Only publication needs sem; scanning must not stall other monitors. */
	down_write(&monitor->sem);
	monitor_mark(&inodes, !!arg);
	if (arg) {
		set_inode_flag(inode, FI_MONITOR_ROOT);
		hash_add(monitor->roots, &config->node, inode->i_ino);
		config = NULL; /* table owns this reference */
	} else {
		clear_inode_flag(inode, FI_MONITOR_ROOT);
		hash_del(&config->node);
		monitor_get(config);
		cancelled = config;
		remove->config = config; /* transfer the table reference */
		remove->remove = true;
		monitor_queue(remove);
		remove = NULL;
		config = NULL;
	}
	up_write(&monitor->sem);
out_inodes:
	monitor_free_inodes(&inodes);
	/* A failed disable still leaves the table owning its config. */
	if (!arg)
		config = NULL;
out_put:
	if (log)
		filp_close(log, NULL);
	if (err && created) {
		int cleanup = monitor_remove_log(config);

		if (cleanup)
			monitor_error(config, cleanup);
	}
	if (config)
		monitor_put(config);
	kfree(remove);
	if (cancelled) {
		/*
		 * Hold config_lock until removal completes to bound removal records
		 * to one and exclude re-enabling. No sem or VFS locks are held here.
		 */
		flush_work(&monitor->work);
		err = READ_ONCE(cancelled->error);
		monitor_put(cancelled);
	}
out_mutex:
	mutex_unlock(&monitor->config_lock);
	return err;
}

int f2fs_monitor_get(struct file *file, unsigned long arg)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(file));
	u32 enabled;

	down_read(&sbi->monitor->sem);
	enabled = is_inode_flag_set(file_inode(file), FI_MONITORED) &&
		  !!monitor_find_root(file->f_path.dentry);
	up_read(&sbi->monitor->sem);
	return put_user(enabled, (u32 __user *)arg);
}

/* Report the latched error without clearing it; repeated SET cannot reset it. */
int f2fs_monitor_get_error(struct file *file, unsigned long arg)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(file));
	struct monitor_config *config;
	int err;

	flush_work(&sbi->monitor->work);
	down_read(&sbi->monitor->sem);
	config = monitor_find_root(file->f_path.dentry);
	err = config ? READ_ONCE(config->error) : -ENOENT;
	up_read(&sbi->monitor->sem);
	return put_user(err, (s32 __user *)arg);
}

/* VFS calls finish after ->rename() and, on success, its dcache update. */
int f2fs_monitor_prepare_rename(struct dentry *old, struct dentry *new,
		unsigned int flags, void **result)
{
	struct inode *inode = d_inode(old), *victim = d_inode(new);
	struct f2fs_monitor *monitor = F2FS_I_SB(inode)->monitor;
	struct monitor_config *from, *to;
	struct f2fs_monitor_rename *rename;
	struct monitor_record *record;
	bool dir = S_ISDIR(inode->i_mode);
	int err = 0;

	*result = NULL;
	/* Configuration and boundary moves require a quiescent target subtree. */
	if (!is_inode_flag_set(inode, FI_MONITORED) &&
	    !is_inode_flag_set(d_inode(old->d_parent), FI_MONITORED) &&
	    !is_inode_flag_set(d_inode(new->d_parent), FI_MONITORED) &&
	    !(victim && is_inode_flag_set(victim, FI_MONITOR_ROOT)))
		return 0;
	down_write(&monitor->sem);
	if (is_inode_flag_set(inode, FI_MONITOR_ROOT) ||
	    (victim && is_inode_flag_set(victim, FI_MONITOR_ROOT))) {
		err = -EBUSY;
		goto out_unlock;
	}
	from = monitor_find_root(old);
	to = monitor_find_root(new->d_parent);
	if (!from && !to)
		goto out_unlock;
	if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT)) {
		err = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (!dir && !S_ISREG(inode->i_mode))
		goto out_unlock;
	rename = kzalloc(sizeof(*rename), GFP_NOFS);
	if (!rename) {
		err = -ENOMEM;
		goto out_unlock;
	}
	rename->monitor = monitor;
	INIT_LIST_HEAD(&rename->inodes);
	INIT_LIST_HEAD(&rename->records);
	rename->monitored = !!to;
	if (!!from != !!to) {
		err = monitor_collect(inode, &rename->inodes);
		if (err) {
			f2fs_monitor_finish_rename(rename, err);
			return err;
		}
	}
	if (victim && to) {
		record = monitor_record(to, dir ? F2FS_D_DELETE : F2FS_F_DELETE,
					false, new, NULL);
		if (record)
			list_add_tail(&record->list, &rename->records);
	}
	if (from && from == to) {
		record = monitor_record(from, dir ? F2FS_D_RENAME : F2FS_F_RENAME,
					true, new, old);
		if (record)
			list_add_tail(&record->list, &rename->records);
	} else {
		if (from) {
			record = monitor_record(from, dir ? F2FS_D_DELETE : F2FS_F_DELETE,
						false, old, NULL);
			if (record)
				list_add_tail(&record->list, &rename->records);
		}
		if (to) {
			record = monitor_record(to, dir ? F2FS_D_CREATE : F2FS_F_CREATE,
						false, new, NULL);
			if (record)
				list_add_tail(&record->list, &rename->records);
		}
	}
	*result = rename;
	return 0;
out_unlock:
	up_write(&monitor->sem);
	return err;
}

void f2fs_monitor_finish_rename(void *context, int err)
{
	struct f2fs_monitor_rename *rename = context;
	struct monitor_record *record, *next;

	if (!rename)
		return;
	if (!err)
		monitor_mark(&rename->inodes, rename->monitored);
	monitor_free_inodes(&rename->inodes);
	list_for_each_entry_safe(record, next, &rename->records, list) {
		list_del(&record->list);
		if (!err)
			monitor_queue(record);
		else
			monitor_free_record(record);
	}
	up_write(&rename->monitor->sem);
	kfree(rename);
}

int f2fs_monitor_init(struct f2fs_sb_info *sbi)
{
	struct f2fs_monitor *monitor;

	monitor = kzalloc(sizeof(*monitor), GFP_KERNEL);
	if (!monitor)
		return -ENOMEM;
	init_rwsem(&monitor->sem);
	mutex_init(&monitor->config_lock);
	hash_init(monitor->roots);
	spin_lock_init(&monitor->queue_lock);
	INIT_LIST_HEAD(&monitor->pending);
	INIT_WORK(&monitor->work, monitor_work);
	sbi->monitor = monitor;
	return 0;
}

/* Called before generic_shutdown_super() evicts inodes, including root pins. */
void f2fs_monitor_destroy(struct f2fs_sb_info *sbi)
{
	struct f2fs_monitor *monitor = sbi->monitor;
	struct monitor_config *config;
	struct hlist_node *next;
	int bucket;

	if (!monitor)
		return;
	flush_work(&monitor->work);
	hash_for_each_safe(monitor->roots, bucket, next, config, node) {
		hash_del(&config->node);
		clear_inode_flag(config->root, FI_MONITOR_ROOT);
		clear_inode_flag(config->root, FI_MONITORED);
		monitor_put(config);
	}
	sbi->monitor = NULL;
	kfree(monitor);
}
