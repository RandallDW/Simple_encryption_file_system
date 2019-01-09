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
// ENCRYPT AND DECRYPT OPERATIONS
void xcfs_encrypt(unsigned char *data, ssize_t count) {
	ssize_t index = 0;
	for (index = 0; index < count; index++) {
		data[index]++;	
	}
}

void xcfs_decrypt(unsigned char *data, ssize_t count) {
	ssize_t index = 0;
	for (index = 0; index < count; index++) {
		data[index]--;	
	}
}

static ssize_t xcfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	/*
	 * This function should never be called directly.  We need it
	 * to exist, to get past a check in open_check_o_direct(),
	 * which is called from do_last().
	 */
	return -EINVAL;
}

static int xcfs_readpage(struct file *file, struct page *page)
{
	int err = 0;
	struct file *lower_file;
	struct inode *inode;
	char *page_data = NULL;
	struct xcfs_sb_info *xcfsb;
	char *dpage_data;
	mode_t orig_mode;
	mm_segment_t old_fs;
	char* cipher;
	struct page *cipher_page;

	cipher_page = alloc_page(GFP_KERNEL);
  //alloc page for the decrypted data
	xcfsb = NULL;
	dpage_data = NULL;

	if (IS_ERR(cipher_page)){
		err = PTR_ERR(cipher_page);
		goto out;
	}

	cipher = kmap(cipher_page);
  //get a char* to the page data

	BUG_ON(file == NULL);

	lower_file = xcfs_lower_file(file);
	BUG_ON(lower_file == NULL);

	inode = file->f_path.dentry->d_inode;
	page_data = (char *) kmap(page);

	//set the pointer of lower file
	lower_file->f_pos = page_offset(page);
	inode_lock(lower_file->f_path.dentry->d_inode);
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	/*** generic_file_splice_write may call us on a file not opened for
	** reading, so temporarily allow reading.
	**/
	orig_mode = lower_file->f_mode;
	lower_file->f_mode |= FMODE_READ;
	xcfsb = (struct xcfs_sb_info *) file->f_path.dentry->d_sb->s_fs_info;
	err = vfs_read(lower_file, cipher, PAGE_SIZE, &lower_file->f_pos);
  //read into the cipher char from the lower file
 
	lower_file->f_mode = orig_mode;
	set_fs(old_fs);
  //repalace with old_fs
  //
	if (err < 0) {
	 goto out_err;
	}    
	memcpy(page_data, cipher, PAGE_SIZE);
	/* key -> decrypt and vfs_read */
	xcfs_decrypt(page_data, PAGE_SIZE);
  //page_data has decrypted content which is mapped to page

out_err:
	if (err >= 0 && err < PAGE_SIZE) {
		memset(page_data + err, 0, PAGE_SIZE - err);
	}

	inode_unlock(lower_file->f_path.dentry->d_inode);
	if (err < 0) {
	  goto out;
	}
	err = 0;
	/* if vfs_read succeeded above, sync up our times */
	fsstack_copy_attr_atime(inode, lower_file->f_path.dentry->d_inode);
	kunmap(page);
	flush_dcache_page(page);
out :
	kunmap(cipher_page);
	__free_page(cipher_page);
  //unmap both the pages and free

	if (err == 0) {
		SetPageUptodate(page);
	}
	else {
		ClearPageUptodate(page);
	}

	unlock_page(page);
    return err;
}

/* 
 * xcfs_writepage writes page with reference to 
 * writeback_Control wbc
 * Similar to ecryptfs
 */
static int xcfs_writepage(struct page *page, struct writeback_control *wbc)
{
	int err = -EIO;
	struct inode *inode;
	struct inode *lower_inode;
	struct page *lower_page;
	struct address_space *lower_mapping; /* lower inode mapping */
	gfp_t mask;

	char *cipher, *plain;
	BUG_ON(!PageUptodate(page));
	inode = page->mapping->host;
	/* if no lower inode, nothing to do */
	if (!inode || !XCFS_I(inode)) {
		err = 0;
		goto out;
	}
	lower_inode = xcfs_lower_inode(inode);
	lower_mapping = lower_inode->i_mapping;
	/*
	 * find lower page (returns a locked page)
	 *
	 * We turn off __GFP_FS while we look for or create a new lower
	 * page.  This prevents a recursion into the file system code, which
	 * under memory pressure conditions could lead to a deadlock.  This
	 * is similar to how the loop driver behaves (see loop_set_fd in
	 * drivers/block/loop.c).  If we can't find the lower page, we
	 * redirty our page and return "success" so that the VM will call us
	 * again in the (hopefully near) future.
	 */
	mask = mapping_gfp_mask(lower_mapping) & ~(__GFP_FS);
	lower_page = find_or_create_page(lower_mapping, page->index, mask);
	if (!lower_page) {
		err = 0;
		set_page_dirty(page);
		goto out;
	}

	plain = kmap(page);
	cipher = kmap(lower_page);

	memcpy(cipher, plain, PAGE_SIZE);
	xcfs_encrypt(cipher, PAGE_SIZE);
  //encrypt the content of the page
	/* copy page data from our upper page to the lower page */
	copy_highpage(lower_page, page);
	flush_dcache_page(lower_page);
	SetPageUptodate(lower_page);
	set_page_dirty(lower_page);

	/*
	 * Call lower writepage (expects locked page).  However, if we are
	 * called with wbc->for_reclaim, then the VFS/VM just wants to
	 * reclaim our page.  Therefore, we don't need to call the lower
	 * ->writepage: just copy our data to the lower page (already done
	 * above), then mark the lower page dirty and unlock it, and return
	 * success.
	 */
	BUG_ON(!lower_mapping->a_ops->writepage);
	wait_on_page_writeback(lower_page); /* prevent multiple writers */
	clear_page_dirty_for_io(lower_page); /* emulate VFS behavior */
	err = lower_mapping->a_ops->writepage(lower_page, wbc);
	if (err < 0)
		goto out_release;
	if (err == AOP_WRITEPAGE_ACTIVATE) {
		err = 0;
		unlock_page(lower_page);
	}

out_release:
	kunmap(page);
	kunmap(lower_page);
	/* b/c find_or_create_page increased refcnt */
	put_page(lower_page);

out:
	unlock_page(page);
	return err;
}

//similar to ecryptfs
static int xcfs_write_begin(struct file *file,
			struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{

	pgoff_t index = pos >> PAGE_SHIFT;

	struct page *page;
	int rc = 0;

	page = grab_cache_page_write_begin(mapping, index, flags);
  //get the page
	if (!page)
		return -ENOMEM;
	*pagep = page;
	return rc;
}

//encryption of data is done here for mmap
//almost same as read_page 
static int xcfs_write_end(struct file *file,
			struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{


	unsigned from = pos & (PAGE_SIZE - 1);
	unsigned to = from + copied;
	unsigned bytes = to - from;
	struct inode *inode = page->mapping->host;
	struct inode *lower_inode = NULL;
	struct file *lower_file = NULL;
	int err = 0;
	char *page_data = NULL;
	mode_t orig_mode;
	mm_segment_t old_fs;

	struct page *cipher_page;
	char *cipher;
	cipher_page = NULL;


	if (!file || !XCFS_F(file)) {
		err = 0;
		goto out;
	}
	BUG_ON(file == NULL);
	lower_file = xcfs_lower_file(file);
	BUG_ON(lower_file == NULL);
	page_data = (char *) kmap(page);
	
	cipher_page = alloc_page(GFP_KERNEL);
  //alloc a new page and map it to a char*
	cipher = kmap(cipher_page);
	memcpy(cipher, page_data, PAGE_SIZE);
	xcfs_encrypt(cipher, PAGE_SIZE);
	lower_file->f_pos = page_offset(page) + from;
  //set the file position in the lower file
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	orig_mode = lower_file->f_mode;
	lower_file->f_mode |= FMODE_WRITE;
	err = vfs_write(lower_file, cipher+from, bytes, \
				&lower_file->f_pos);

	set_fs(old_fs);
	kunmap(page);

	if (err < 0) {
		printk(KERN_INFO "vfs_write failed\n");
		goto out;
	}
	/*
	 * checking if lower_file has inode and then assigning 
	 * lower_inode the inode from file.
	 */
	lower_inode = lower_file->f_path.dentry->d_inode;
	if (!lower_inode) {
		lower_inode = xcfs_lower_inode(inode);
	}
	BUG_ON(!lower_inode);
	BUG_ON(!inode);
	/* copying inode size and times */
	fsstack_copy_inode_size(inode, lower_inode);
	fsstack_copy_attr_times(inode, lower_inode);
	mark_inode_dirty_sync(inode);
out:
	kunmap(cipher_page);
	__free_page(cipher_page);

	if (err < 0) {
		ClearPageUptodate(page);
	}
	unlock_page(page);
	put_page(page);
	return err;	

}

/* xcfs address space operations */
const struct address_space_operations xcfs_aops = {
	.direct_IO = xcfs_direct_IO,
	.readpage = xcfs_readpage,
	.writepage = xcfs_writepage,
	.write_begin = xcfs_write_begin,
	.write_end = xcfs_write_end,
};

