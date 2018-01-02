/*
 * Copyright (c) 1998-2017 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2017 Stony Brook University
 * Copyright (c) 2003-2017 The Research Foundation of SUNY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "xcfs.h"
/*read and write are not used, generic_read_iter and write_iter call read_page
 * and writepage*/
static ssize_t xcfs_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	int err;
	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;
	lower_file = xcfs_lower_file(file);
	err = vfs_read(lower_file, buf, count, ppos);
	/* update our inode atime upon a successful lower read */
	if (err >= 0)
		fsstack_copy_attr_atime(d_inode(dentry),
					file_inode(lower_file));
	return err;
}

static ssize_t xcfs_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	int err;
	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = xcfs_lower_file(file);
	err = vfs_write(lower_file, buf, count, ppos);
	/* update our inode times+sizes upon a successful lower write */
	if (err >= 0) {
		fsstack_copy_inode_size(d_inode(dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(dentry),
					file_inode(lower_file));
	}
	return err;
}

static int xcfs_readdir(struct file *file, struct dir_context *ctx)
{
	int err;
	struct file *lower_file = NULL;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = xcfs_lower_file(file);
	err = iterate_dir(lower_file, ctx);
	file->f_pos = lower_file->f_pos;
	if (err >= 0)		/* copy the atime */
		fsstack_copy_attr_atime(d_inode(dentry),
					file_inode(lower_file));
	return err;
}

static long xcfs_unlocked_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;
	lower_file = xcfs_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->unlocked_ioctl)
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

	/* some ioctls can change inode attributes (EXT2_IOC_SETFLAGS) */
	if (!err)
		fsstack_copy_attr_all(file_inode(file),
				      file_inode(lower_file));
out:
	return err;
}

#ifdef CONFIG_COMPAT
static long xcfs_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = xcfs_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

out:
	return err;
}
#endif

static int xcfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *lower_file;

	lower_file = xcfs_lower_file(file);
	if(!lower_file->f_op->mmap) {
		return -ENODEV;
	}
	return generic_file_mmap(file, vma);
}

static int xcfs_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_path;
	/* don't open unhashed/deleted files */
	if (d_unhashed(file->f_path.dentry)) {
		err = -ENOENT;
		goto out_err;
	}

	file->private_data =
		kzalloc(sizeof(struct xcfs_file_info), GFP_KERNEL);
	if (!XCFS_F(file)) {
		err = -ENOMEM;
		goto out_err;
	}

	/* open lower object and link xcfs's file struct to lower's */
	xcfs_get_lower_path(file->f_path.dentry, &lower_path);
	lower_file = dentry_open(&lower_path, file->f_flags, current_cred());
	path_put(&lower_path);
	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		lower_file = xcfs_lower_file(file);
		if (lower_file) {
			xcfs_set_lower_file(file, NULL);
			fput(lower_file); /* fput calls dput for lower_dentry */
		}
	} else {
		xcfs_set_lower_file(file, lower_file);
	}

	if (err)
		kfree(XCFS_F(file));
	else
		fsstack_copy_attr_all(inode, xcfs_lower_inode(inode));
out_err:
	return err;
}

static int xcfs_flush(struct file *file, fl_owner_t id)
{
	int err = 0;
	struct file *lower_file = NULL;
  
	lower_file = xcfs_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush) {
		filemap_write_and_wait(file->f_mapping);
		err = lower_file->f_op->flush(lower_file, id);
	}
	return err;
}

/* release all lower object references & free the file info structure */
static int xcfs_file_release(struct inode *inode, struct file *file)
{
	struct file *lower_file;
	lower_file = xcfs_lower_file(file);
	if (lower_file) {
		xcfs_set_lower_file(file, NULL);
		fput(lower_file);
	}

	kfree(XCFS_F(file));
	return 0;
}

//calls lower file fsync operation
static int xcfs_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	int err;
	struct file *lower_file;
	struct path lower_path;
	struct dentry *dentry = file->f_path.dentry;

	err = __generic_file_fsync(file, start, end, datasync);
	if (err)
		goto out;
	lower_file = xcfs_lower_file(file);
	xcfs_get_lower_path(dentry, &lower_path);
	err = vfs_fsync_range(lower_file, start, end, datasync);
	xcfs_put_lower_path(dentry, &lower_path);
out:
	return err;
}

//calls lower file async operation
static int xcfs_fasync(int fd, struct file *file, int flag)
{
	int err = 0;
	struct file *lower_file = NULL;
	lower_file = xcfs_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		err = lower_file->f_op->fasync(fd, lower_file, flag);
	return err;
}

/*
 * xcfs cannot use generic_file_llseek as ->llseek, because it would
 * only set the offset of the upper file.  So we have to implement our
 * own method to set both the upper and lower file offsets
 * consistently.
 */
static loff_t xcfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	int err;
	struct file *lower_file;
	err = generic_file_llseek(file, offset, whence);
	if (err < 0)
		goto out;

	lower_file = xcfs_lower_file(file);
	err = generic_file_llseek(lower_file, offset, whence);

out:
	return err;
}

/*
 * xcfs read_iter, redirect modified iocb to lower read_iter
 */
ssize_t
xcfs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp, *lower_file;
	lower_file = xcfs_lower_file(file);
	if (!lower_file->f_op->read_iter) {
		err = -EINVAL;
		goto out;
	}

	get_file(lower_file); /* prevent lower_file from being released */
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
 * xcfs write_iter, redirect modified iocb to lower write_iter
 */
ssize_t
xcfs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp, *lower_file;
	lower_file = xcfs_lower_file(file);
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

//we are not using this fops
const struct file_operations xcfs_main_fops = {
	.llseek		= generic_file_llseek,
	.read		= xcfs_read,
	.write		= xcfs_write,
	.unlocked_ioctl	= xcfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xcfs_compat_ioctl,
#endif
	.mmap		= xcfs_mmap,
	.open		= xcfs_open,
	.flush		= xcfs_flush,
	.release	= xcfs_file_release,
	.fsync		= xcfs_fsync,
	.fasync		= xcfs_fasync,
	.read_iter	= xcfs_read_iter,
	.write_iter	= xcfs_write_iter,
};

// NEW fops added
const struct file_operations xcfs_mmap_fops = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.unlocked_ioctl	= xcfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xcfs_compat_ioctl,
#endif
	.mmap		= xcfs_mmap,
	.open		= xcfs_open,
	.flush		= xcfs_flush,
	.release	= xcfs_file_release,
	.fsync		= xcfs_fsync,
	.fasync		= xcfs_fasync,
};

/* trimmed directory options */
const struct file_operations xcfs_dir_fops = {
	.llseek		= xcfs_file_llseek,
	.read		= generic_read_dir,
	.iterate	= xcfs_readdir,
	.unlocked_ioctl	= xcfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xcfs_compat_ioctl,
#endif
	.open		= xcfs_open,
	.release	= xcfs_file_release,
	.flush		= xcfs_flush,
	.fsync		= xcfs_fsync,
	.fasync		= xcfs_fasync,
};
