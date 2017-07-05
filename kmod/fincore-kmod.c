/**
 * fincore-kmod.c - Helper kernel module to implement fincore() call to get the
 *		    the set of in-core pages of a file.
 * 
 * Author: Sougata Santra <sougata.santra@gmail.com>
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/pagevec.h>
#include <linux/file.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

static struct dentry *dentry_root, *dentry_params;

/*
 * Some enums to define the page flags of interest which we can piggyback into a
 * byte of information we send to user-space for one page of in-core page.
 */
enum {
	FPG_uptodate = 0,
	FPG_active,
	FPG_referenced,
	FPG_dirty,
	FPG_writeback,
	FPG_unevicatable,
	FPG_reclaim,
	FPG_private,
	FINCORE_MAX_ARGUMENT_SIZE = 120 /* Max size of fincore arguments
					 * converted to string.  This should
					 * be enough to pass four u64
					 * separated by space and argument
					 * as string terminated by '\0". */
};

/**
 * Fincore info structure for the fincore request.
 */
typedef struct {
	unsigned fd;				/* File descriptor for the file
						   for which we have to get the
						   in-core pages. */
	loff_t start;				/* Start offset of the file
						   rounded down to
						   @PAGE_SIZE. */
	size_t length;				/* Length of the file in
						   multiple of page size. */
	__user void *ptr;			/* User-space address were we
						   pass the fincore result
						   vector. */
	char buffer[FINCORE_MAX_ARGUMENT_SIZE]; /* Buffer where we store the
						   passed fincore arguments
						   written into the debug
						   file. */
	struct mutex lock;			/* Lock to synchronize mutual
						   access of @buffer. */
} fincore_info;

static inline int unsigned_offsets(struct file *file)
{
	return file->f_mode & FMODE_UNSIGNED_OFFSET;
}

#define err_str(str) FINCORE_MODULE_NAME": Invalid "str" argument passed. "\
		"Aborting.\n"
/**
 * fincore_parse_options - parse fincore arguments passed through the
 *		debug file write handler.
 * @info	- structure describing this fincore request.
 *
 * The arguments in @info->buffer should be interpreted as four arguments:
 *
 *   i) A unsigned file-descriptor of the file for which we have to get the
 *	the in-core pages from corresponding inodes address-space mapping.
 *  ii) The offset within the file to determine the page index from which
 *	we begin our search.
 * iii) The length of in bytes for the search.
 *  iv) An user-space linear address in which to return the results of the
 *	of the search.  A byte for each page starting at @start and up-to
 *	@length bytes.
 * Returns @true if all the arguments for the fincore call is found in found
 * @info->buffer, false otherwise.
 */
static bool fincore_parse_options(fincore_info *info)
{
	int err, argc;
	char *tok, *end;

	argc = 0;
	end = tok = info->buffer;
	while (tok) {
		char *endp;
		long fd;
		u64 length, ptr, start;

		strsep(&end, " ");
		switch (argc++) {
			case 0:
				fd = simple_strtol(tok, &endp, 0);
				if (endp == tok || (fd < (long)INT_MIN ||
						fd > (long)INT_MAX)) {
					pr_warn(err_str("file descriptor"));
					goto err_out;
				}
				info->fd = (int) fd;
				break;
			case 1:
				err =  kstrtoll(tok, 0, &start);
				if (err) {
					pr_warn(err_str("start"));
					goto err_out;
				}
				info->start = (int) start;
				break;
			case 2:
				err = kstrtoll(tok, 0, &length);
				if (err) {
					pr_warn(err_str("length"));
					goto err_out;
				}
				info->length = length;
				break;
			case 3:
				err = kstrtoull(tok, 16, &ptr);
				if (err) {
					pr_warn(err_str("address"));
					goto err_out;
				}
				if (!access_ok(VERIFY_WRITE, __user (void *)ptr,
							length)) {
					pr_warn (FINCORE_MODULE_NAME ": Passed "
							"buffer is not "
							"accessible.\n");
					goto err_out;
				}
				info->ptr = (void *)ptr;
				break;
			default:
				goto invalid_args;
		}
		tok = end;
	}
	if (argc < 3) {
invalid_args:
		pr_warn_once(FINCORE_MODULE_NAME ": Invalid "
				"number of arguments. ");
		goto err_out;
	}
	return true;
err_out:
	return false;
}

static unsigned char fincore_page_flags(struct page *page)
{
	unsigned char flags;

	flags = 0x0;
	if (!PageUptodate(page))
		return flags;
	/*
	 * TODO:
	 * 	This can be improved.
	 */
	flags |= (1 << FPG_uptodate);
	if (PageActive(page))
		flags |= (1 << FPG_active);
	if (PageReferenced(page))
		flags |= (1 << FPG_referenced);
	if (PageDirty(page))
		flags |= (1 << FPG_dirty);
	if (PageWriteback(page))
		flags |= (1 << FPG_writeback);
	if (PageUnevictable(page))
		flags |= (1 << FPG_unevicatable);
	if (PageReclaim(page))
		flags |= (1 << FPG_reclaim);
	if (page_has_buffers(page))
		flags |= (1 << FPG_private);
	return flags;
}

/**
 * do_fincore - Walk through the address-space mapping and check pages which
 *		are uptodate.
 * @mappping:	the address-space mapping to walk
 * @pgstart:	page index at which we begin our walk
 * @nr_pages:	the max number of pages we are trying to find in this walk
 * @vec:	buffer to store result for each page we find in the walk.
 *
 * We do a radix-tree gang lookup and try to find if the pages starting at
 * index @pgstart and upto max nr_pages are are up-todate then piggyback some
 * other interesting page flags in the same byte of information for the page.
 *
 * Returns the number of pages searched starting from @pgstart.
 */
static pgoff_t do_fincore(struct address_space *mapping, pgoff_t pgstart,
			unsigned long nr_pages, unsigned char *vec)
{
	int i;
	pgoff_t length;
	struct pagevec pvec;
	struct page *page;

	length = 0;
	rcu_read_lock();
	do {
		pagevec_init(&pvec, 0);
		pvec.nr = radix_tree_gang_lookup(&mapping->page_tree,
				(void **)pvec.pages, pgstart + length,
				PAGEVEC_SIZE);
		if (!pvec.nr)
			break;
		for (i = 0; i < pvec.nr; i++) {
			int index;

			page = pvec.pages[i];
			index = page->index - pgstart;
			if (index > nr_pages - 1)
				break;
			vec[index] = fincore_page_flags(page);
			length = index + 1;
			if (length == nr_pages)
				break;
		}
	} while(length <= nr_pages);
	rcu_read_unlock();
	return length;
}

/**
 * fincore_file_write - write handler for debug file
 * @file:	file on which to write
 * @buf:	user-space buffer to be written
 * @len:	bytes to write
 * @ppos:	stat offset of the write.
 *
 * We interpret the written buffer as arguments for fincore call. First
 * we parse the arguments to set the values for @fincore_info.fd
 * @fincore_info.start, @fincore_info.length and fincore_info.ptr.
 *
 * fincore() returns the memory residency status of the given file's
 * pages, in the range [start, start + length].  The status is returned in a
 * vector of bytes.  The least significant  bit of each byte is 1 if the
 * referenced page is in memory, otherwise it is zero.
 *
 * Because the status of a page can change after fincore() checks it
 * but before it returns to the application, the returned vector may
 * contain stale information.
 *
 * Return written bytes on success and -errno on error.
 */
ssize_t fincore_file_write(struct file *file, const char __user *buf,
		size_t len, loff_t *ppos)
{
	size_t size;
	ssize_t ret;
	pgoff_t pgstart;
	unsigned long nr_pages;
	fincore_info *info;
	struct fd f;
	struct page *page;
	unsigned char *page_status_vec;

	page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
	if (!page)
		return -ENOMEM;
	page_status_vec = kmap(page);
	info = file->private_data;
	ret = mutex_lock_interruptible(&info->lock);
	if (ret) {
		kunmap(page);
		__free_page(page);
		return ret;
	}
	if (*ppos) {
		pr_warn_once(FINCORE_MODULE_NAME ": Wrote to file %s"
				"when file position was not 0!. Aborting\n",
				FINCORE_MODULE_NAME"-params");
		ret = -EFAULT;
		goto out;
	}
	size = min(sizeof(info->buffer) - 1, len);
	if (copy_from_user(info->buffer, buf, size))
		goto out;
	info->buffer[size] = '\0';
	/*
	 * Parse the arguments for this fincore request written to
	 * fincore debug file.
	 */
	if (!fincore_parse_options(info)) {
		ret = -EINVAL;
		goto out;
	}
	printk ("fd: %u, start %llu, length %zu, ptr %p\n", info->fd,
			info->start, info->length, info->ptr);
	f = fdget(info->fd);
	if (!f.file) {
		ret = -EBADF;
		goto out;
	}
	/* Few extra sanity checks before we proceed. */
	if (unlikely(info->start < 0)) {
		if (!unsigned_offsets(f.file)) {
			ret = -EINVAL;
			goto out;
		}
		if (info->length >= -info->start) {
			ret = -EOVERFLOW;
			goto out;
		}
	} else if (unlikely((loff_t) (info->start + info->length) < 0)) {
		if (!unsigned_offsets(f.file)) {
			ret = -EINVAL;
			goto out;
		}
	}
	pgstart = info->start >> PAGE_SHIFT;
	nr_pages = DIV_ROUND_UP(info->length, PAGE_SIZE);
	while (nr_pages) {

		memset(page_status_vec, 0, PAGE_SIZE);
		ret = do_fincore(f.file->f_mapping, pgstart,
				min(nr_pages, PAGE_SIZE),
				page_status_vec);
		if (ret <= 0)
			break;
		if (copy_to_user(info->ptr, page_status_vec, ret)) {
			ret = -EFAULT;
			break;
		}
		nr_pages -= ret;
		pgstart += ret;
		info->ptr += ret;
		ret = 0;
	}
	fdput(f);
	ret = len;
out:
	mutex_unlock(&info->lock);
	kunmap(page);
	__free_page(page);
	return ret;
}

/**
 * fincore_file_open - called when an inode is about to be opened
 * @indoe:	inode to be opened
 * @filp:	file structure describing the inode
 *
 * After allocating fincore info, initializing it and setting it to
 * @filp->private data we just call nonseekable_open() to do rest
 * of the work.
 */
int fincore_file_open(struct inode *inode, struct file *filp)
{
	fincore_info *info;

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	mutex_init(&info->lock);
	info->fd = 0;
	info->start = -1;
	info->length = 0;
	/*
	 * Need to check if sparse throws an warning here, we are trying to
	 * force a kernel space address (hole) into an user-space address
	 * pointer.
	 */
	info->ptr = (__force void *)(PAGE_OFFSET + PAGE_SIZE);
	filp->private_data = info;
	return nonseekable_open(inode, filp);
}

/**
 * fincore_file_release - Release @filp->private data
 * @inode:	inode corresponding to the file being closed
 * @filp:	file being closed.
 */
int fincore_file_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static const struct file_operations fincore_fops_params = {
	.owner = THIS_MODULE,
	.open = fincore_file_open,
	.release = fincore_file_release,
	.write = fincore_file_write,
};

static int __init fincore_module_init(void)
{
	int err;

	err = 0;
	dentry_root = debugfs_create_dir(FINCORE_MODULE_NAME"-dir", NULL);
	if (IS_ERR(dentry_root) || !dentry_root) {
		if (IS_ERR(dentry_root))
			err = PTR_ERR(dentry_root);
		pr_warn(FINCORE_MODULE_NAME ": Failed to create directory %s\n",
			FINCORE_MODULE_NAME"-dir");
		goto err_root;
	}
	dentry_params = debugfs_create_file("params", 0222, dentry_root, NULL,
			&fincore_fops_params);
	if (IS_ERR(dentry_params) || !dentry_params) {
		if (IS_ERR(dentry_params))
			err =  PTR_ERR(dentry_params);
		pr_warn(FINCORE_MODULE_NAME ": Failed to create file %s\n",
			FINCORE_MODULE_NAME"-params");
		goto err_params;
	}
	pr_info(FINCORE_MODULE_NAME ": Insmoded successfully!\n");
	return 0;
err_params:
	debugfs_remove(dentry_root);
err_root:
	return err;
}

static void __exit fincore_module_exit(void)
{
	debugfs_remove(dentry_params);
	debugfs_remove(dentry_root);
}

module_init(fincore_module_init);
module_exit(fincore_module_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sougata Santra");
