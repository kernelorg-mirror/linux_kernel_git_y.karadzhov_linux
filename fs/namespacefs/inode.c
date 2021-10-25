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

struct dentry *namespacefs_create_file(const char *name,
				       struct dentry *parent,
				       const struct file_operations *fops,
				       void *data)
{
	return _create(name, parent, fops, data);
}

struct dentry *namespacefs_create_dir(const char *name,
				      struct dentry *parent)
{
	return _create(name, parent, NULL, NULL);
}

static void _remove_one(struct dentry *d)
{
	_release_namespacefs();
}

void namespacefs_remove_dir(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry))
		return;

	if (_pin_fs())
		return;

	simple_recursive_removal(dentry, _remove_one);
	_release_namespacefs();
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

	namespacefs_registered = true;
	return 0;

 fail:
	return err;
}

fs_initcall(namespacefs_init);
