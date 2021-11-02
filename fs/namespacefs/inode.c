// SPDX-License-Identifier: GPL-2.0-only
/*
 * inode.c - part of namespacefs, pseudo filesystem for examining namespaces.
 *
 * Copyright 2021 VMware Inc, Yordan Karadzhov (VMware) <y.karadz@gmail.com>
 */

#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/namei.h>
#include <linux/fsnotify.h>
#include <linux/magic.h>
#include <linux/idr.h>
#include <linux/proc_ns.h>
#include <linux/seq_file.h>
#include <linux/pid_namespace.h>
#include <linux/utsname.h>

#define S_IRALL (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IXALL (S_IXUSR | S_IXGRP | S_IXOTH)

static struct vfsmount *namespacefs_mount;
static int namespacefs_mount_count;
static bool namespacefs_registered;

static const struct super_operations namespacefs_super_operations = {
	.statfs		= simple_statfs,
};

static int _fill_super(struct super_block *sb, void *data, int silent)
{
	static const struct tree_descr files[] = {{""}};
	int err;

	err = simple_fill_super(sb, NAMESPACEFS_MAGIC, files);
	if (err)
		return err;

	sb->s_op = &namespacefs_super_operations;
	sb->s_root->d_inode->i_mode |= S_IRALL;

	return 0;
}

static struct dentry *_namespacefs_mount(struct file_system_type *fs_type,
					 int flags, const char *dev_name,
					 void *data)
{
	return mount_single(fs_type, flags, data, _fill_super);
}

static struct file_system_type namespacefs_fs_type = {
	.name		= "namespacefs",
	.mount		= _namespacefs_mount,
	.kill_sb	= kill_litter_super,
	.fs_flags	= FS_USERNS_MOUNT,
};

static inline void _release_namespacefs(void)
{
	simple_release_fs(&namespacefs_mount, &namespacefs_mount_count);
}

static inline struct inode *_parent_inode(struct dentry *dentry)
{
	return dentry->d_parent->d_inode;
}

static struct inode *_get_inode(struct super_block *sb)
{
	struct inode *inode = new_inode(sb);
	if (inode) {
		inode->i_ino = get_next_ino();
		inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	}
	return inode;
}

static inline void _set_file_inode(struct inode *inode,
				   void *data, const struct file_operations *fops)
{
	inode->i_fop = fops;
	inode->i_private = data;
	inode->i_mode = S_IFREG | S_IRALL;
}

static inline void _set_dir_inode(struct inode *inode)
{
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	inode->i_mode = S_IFDIR | S_IRWXU | S_IXALL | S_IRALL;
}

static inline int _pin_fs(void)
{
	return simple_pin_fs(&namespacefs_fs_type,
			     &namespacefs_mount,
			     &namespacefs_mount_count);
}

static struct dentry *_create(const char *name, struct dentry *parent,
			      const struct file_operations *fops, void *data)
{
	struct dentry *dentry = NULL;
	struct inode *inode;

	if (_pin_fs())
		return ERR_PTR(-ESTALE);

	/*
	 * If the parent is not specified, we create it in the root.
	 * We need the root dentry to do this, which is in the super
	 * block. A pointer to that is in the struct vfsmount that we
	 * have around.
	 */
	if (!parent)
		parent = namespacefs_mount->mnt_root;

	inode_lock(parent->d_inode);
	if (unlikely(IS_DEADDIR(parent->d_inode)))
		return ERR_PTR(-ESTALE);

	dentry = lookup_one_len(name, parent, strlen(name));
	if (IS_ERR(dentry) || (!IS_ERR(dentry) && dentry->d_inode))
		goto fail;

	inode = _get_inode(dentry->d_sb);
	if (unlikely(!inode))
		goto fail;

	if (fops) {
		/* Create a file. */
		_set_file_inode(inode, data, fops);
		d_instantiate(dentry, inode);
		fsnotify_create(_parent_inode(dentry), dentry);
	} else {
		/* Create a directory. */
		_set_dir_inode(inode);
		d_instantiate(dentry, inode);
		fsnotify_mkdir(_parent_inode(dentry), dentry);
	}

	inode_unlock(_parent_inode(dentry));
	return dentry;

 fail:
	if(!IS_ERR_OR_NULL(dentry))
		dput(dentry);

	inode_unlock(parent->d_inode);
	_release_namespacefs();

	return ERR_PTR(-ESTALE);
}

static struct dentry * namespacefs_create_file(const char *name,
					       struct dentry *parent,
					       const struct file_operations *fops,
					       void *data)
{
	return _create(name, parent, fops, data);
}

static struct dentry *namespacefs_create_dir(const char *name,
					     struct dentry *parent)
{
	return _create(name, parent, NULL, NULL);
}

static void _remove_one(struct dentry *d)
{
	_release_namespacefs();
}

static void namespacefs_remove_dir(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry))
		return;

	if (_pin_fs())
		return;

	simple_recursive_removal(dentry, _remove_one);
	_release_namespacefs();
}

struct idr_seq_context {
	struct idr	*idr;
	int		index;
};

static struct idr_seq_context *_alloc_idr_seq_context(struct idr *idr)
{
	struct idr_seq_context *idr_ctx = kzalloc(sizeof(*idr_ctx), GFP_KERNEL);

	if (idr_ctx)
		idr_ctx->idr = idr;
	return idr_ctx;
}

static void *_idr_seq_get_next(struct idr_seq_context *idr_ctx, loff_t *pos)
{
	void *next = idr_get_next(idr_ctx->idr, &idr_ctx->index);

	*pos = ++idr_ctx->index;
	return next;
}
static void *idr_seq_start(struct seq_file *m, loff_t *pos)
{
	struct idr_seq_context *idr_ctx = m->private;

	idr_lock(idr_ctx->idr);
	idr_ctx->index = *pos;
	return _idr_seq_get_next(idr_ctx, pos);
}

static void *idr_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return _idr_seq_get_next(m->private, pos);
}

static void idr_seq_stop(struct seq_file *m, void *p)
{
	struct idr_seq_context *idr_ctx = m->private;

	idr_unlock(idr_ctx->idr);
}

static int idr_seq_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	kfree(m->private);
	return seq_release(inode, file);
}

static int idr_seq_open(struct file *file, struct idr *idr,
			const struct seq_operations *ops)
{
	struct seq_file *m;
	int ret;

	ret = seq_open(file, ops);
	if (ret)
		return ret;

	m = file->private_data;
	m->private = _alloc_idr_seq_context(idr);
	if (!m->private)
		return -ENOMEM;

	return 0;
}

static inline int pid_seq_show(struct seq_file *m, void *v)
{
	struct pid *pid = v;
	seq_printf(m, "%d\n", pid_nr(pid));
	return 0;
}

static const struct seq_operations pid_seq_ops = {
	.start		= idr_seq_start,
	.next		= idr_seq_next,
	.stop		= idr_seq_stop,
	.show		= pid_seq_show,
};

static int pid_seq_open(struct inode *inode, struct file *file)
{
	struct idr *idr = inode->i_private;
	return idr_seq_open(file, idr, &pid_seq_ops);
}

static const struct file_operations tasks_fops = {
	.open		= pid_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= idr_seq_release,
};

static int _create_inod_dir(struct ns_common *ns, struct dentry *parent_dentry)
{
	char *dir = kasprintf(GFP_KERNEL, "%u", ns->inum);

	if (!dir)
		return -ENOMEM;

	ns->dentry = namespacefs_create_dir(dir, parent_dentry);
	kfree(dir);
	if (IS_ERR(ns->dentry))
		return PTR_ERR(ns->dentry);

	return 0;
}

int namespacefs_create_pid_ns_dir(struct pid_namespace *ns)
{
	int err = _create_inod_dir(&ns->ns, ns->parent->ns.dentry);
	struct dentry *dentry;

	if (err)
		return err;

	dentry = namespacefs_create_file("tasks", ns->ns.dentry,
					 &tasks_fops, &ns->idr);
	if (IS_ERR(dentry)) {
		dput(ns->ns.dentry);
		return PTR_ERR(dentry);
	}

	return 0;
}

void namespacefs_remove_pid_ns_dir(struct pid_namespace *ns)
{
	namespacefs_remove_dir(ns->ns.dentry);
}

#define _UNAME_N_FIELDS		5
#define _UNAME_MAX_LEN		((__NEW_UTS_LEN + 2) * _UNAME_MAX_LEN + 1)

static ssize_t _uts_ns_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *pos)
{
	struct new_utsname *name = file->private_data;
	char buff[_UNAME_MAX_LEN];
	int n;

	n = snprintf(buff, _UNAME_MAX_LEN,
		     "%s %s %s %s %s\n",
		     name->sysname,
		     name->nodename,
		     name->release,
		     name->version,
		     name->machine);

	return simple_read_from_buffer(ubuf, count, pos, buff, n);
}

static const struct file_operations uts_fops = {
	.open = simple_open,
	.read = _uts_ns_read,
	.llseek = default_llseek,
};

int namespacefs_create_uts_ns_dir(struct uts_namespace *ns)
{
	int err = _create_inod_dir(&ns->ns, init_uts_ns.ns.dentry);
	struct dentry *dentry;

	if (err)
		return err;

	dentry = namespacefs_create_file("uname", ns->ns.dentry,
					 &uts_fops, &ns->name);
	if (IS_ERR(dentry)) {
		dput(ns->ns.dentry);
		return PTR_ERR(dentry);
	}

	return 0;
}

void namespacefs_remove_uts_ns_dir(struct uts_namespace *ns)
{
	namespacefs_remove_dir(ns->ns.dentry);
}

static int _add_ns_dentry(struct ns_common *ns)
{
	struct dentry *dentry = namespacefs_create_dir(ns->ops->name, NULL);

	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	ns->dentry = dentry;

	return 0;
}

static int __init namespacefs_init(void)
{
	int err;

	err = sysfs_create_mount_point(fs_kobj, "namespaces");
	if (err)
		goto fail;

	err = register_filesystem(&namespacefs_fs_type);
	if (err)
		goto fail;

	err = _add_ns_dentry(&init_pid_ns.ns);
	if (err)
		goto unreg;

	err = _add_ns_dentry(&init_uts_ns.ns);
	if (err)
		goto unreg;

	namespacefs_registered = true;
	return 0;

 unreg:
	unregister_filesystem(&namespacefs_fs_type);
 fail:
	return err;
}

fs_initcall(namespacefs_init);
