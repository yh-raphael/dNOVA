/*
 * BRIEF DESCRIPTION
 *
 * File operations for files.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/falloc.h>
#include <asm/mman.h>
#include "nova.h"
#include "inode.h"
#include "dedup.h"

/*** DEDUP ***/
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/crypto.h>

// DEDUP //
static int nova_dedup(struct file *filp) {
	printk("nova_dedup() called in fs/nova/file.c \n");
printk("sizeof(struct file*): %ld \n", sizeof(struct file *));
printk("sizeof(struct file): %ld \n", sizeof(struct file));
//	struct address_space *mapping = filp->f_mapping;		///////
//	struct inode *inode = mapping->host;			///////

//	sb_start_write(inode->i_sb);	///////
//	inode_lock(inode);		///////

	nova_dedup_test(filp);	// let's test it!

//	inode_unlock(inode);		///////
//	sb_end_write(inode->i_sb);	///////

	return 7;
}
static int test_hash(const unsigned char *data, unsigned int datalen, unsigned char *digest);
// ----- //

static inline int nova_can_set_blocksize_hint(struct inode *inode,
	struct nova_inode *pi, loff_t new_size)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;

	/* Currently, we don't deallocate data blocks till the file is deleted.
	 * So no changing blocksize hints once allocation is done.
	 */
	if (sih->i_size > 0)
		return 0;
	return 1;
}

int nova_set_blocksize_hint(struct super_block *sb, struct inode *inode,
	struct nova_inode *pi, loff_t new_size)
{
	unsigned short block_type;
	unsigned long irq_flags = 0;

	if (!nova_can_set_blocksize_hint(inode, pi, new_size))
		return 0;

	if (new_size >= 0x40000000) {   /* 1G */
		block_type = NOVA_BLOCK_TYPE_1G;
		goto hint_set;
	}

	if (new_size >= 0x200000) {     /* 2M */
		block_type = NOVA_BLOCK_TYPE_2M;
		goto hint_set;
	}

	/* defaulting to 4K */
	block_type = NOVA_BLOCK_TYPE_4K;

hint_set:
	nova_dbg_verbose(
		"Hint: new_size 0x%llx, i_size 0x%llx\n",
		new_size, pi->i_size);
	nova_dbg_verbose("Setting the hint to 0x%x\n", block_type);
	nova_memunlock_inode(sb, pi, &irq_flags);
	pi->i_blk_type = block_type;
	nova_memlock_inode(sb, pi, &irq_flags);
	return 0;
}

static loff_t nova_llseek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	int retval;

	if (origin != SEEK_DATA && origin != SEEK_HOLE)
		return generic_file_llseek(file, offset, origin);

	inode_lock(inode);
	switch (origin) {
	case SEEK_DATA:
		retval = nova_find_region(inode, &offset, 0);
		if (retval) {
			inode_unlock(inode);
			return retval;
		}
		break;
	case SEEK_HOLE:
		retval = nova_find_region(inode, &offset, 1);
		if (retval) {
			inode_unlock(inode);
			return retval;
		}
		break;
	}

	if ((offset < 0 && !(file->f_mode & FMODE_UNSIGNED_OFFSET)) ||
	    offset > inode->i_sb->s_maxbytes) {
		inode_unlock(inode);
		return -ENXIO;
	}

	if (offset != file->f_pos) {
		file->f_pos = offset;
		file->f_version = 0;
	}

	inode_unlock(inode);
	return offset;
}

/* This function is called by both msync() and fsync().
 * TODO: Check if we can avoid calling nova_flush_buffer() for fsync. We use
 * movnti to write data to files, so we may want to avoid doing unnecessary
 * nova_flush_buffer() on fsync()
 */
static int nova_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	unsigned long start_pgoff, end_pgoff;
	int ret = 0;
	INIT_TIMING(fsync_time);

	NOVA_START_TIMING(fsync_t, fsync_time);

	if (datasync)
		NOVA_STATS_ADD(fdatasync, 1);

	/* No need to flush if the file is not mmaped */
	if (!mapping_mapped(mapping))
		goto persist;

	start_pgoff = start >> PAGE_SHIFT;
	end_pgoff = (end + 1) >> PAGE_SHIFT;
	nova_dbgv("%s: msync pgoff range %lu to %lu\n",
			__func__, start_pgoff, end_pgoff);

	/*
	 * Set csum and parity.
	 * We do not protect data integrity during mmap, but we have to
	 * update csum here since msync clears dirty bit.
	 */
	nova_reset_mapping_csum_parity(sb, inode, mapping,
					start_pgoff, end_pgoff);

	ret = generic_file_fsync(file, start, end, datasync);

persist:
	PERSISTENT_BARRIER();
	NOVA_END_TIMING(fsync_t, fsync_time);

	return ret;
}

/* This callback is called when a file is closed */
static int nova_flush(struct file *file, fl_owner_t id)
{
	PERSISTENT_BARRIER();
	return 0;
}

static int nova_open(struct inode *inode, struct file *filp)
{
	printk("open file success! \n");
	return generic_file_open(inode, filp);
}

static long nova_fallocate(struct file *file, int mode, loff_t offset,
	loff_t len)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_inode *pi;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	unsigned long start_blk, num_blocks, ent_blks = 0;
	unsigned long total_blocks = 0;
	unsigned long blocknr = 0;
	unsigned long blockoff;
	unsigned int data_bits;
	loff_t new_size;
	long ret = 0;
	int inplace = 0;
	int blocksize_mask;
	int allocated = 0;
	bool update_log = false;
	INIT_TIMING(fallocate_time);
	u64 begin_tail = 0;
	u64 epoch_id;
	u32 time;
	unsigned long irq_flags = 0;

	/*
	 * Fallocate does not make much sence for CoW,
	 * but we still support it for DAX-mmap purpose.
	 */

	/* We only support the FALLOC_FL_KEEP_SIZE mode */
	if (mode & ~FALLOC_FL_KEEP_SIZE)
		return -EOPNOTSUPP;

	if (S_ISDIR(inode->i_mode))
		return -ENODEV;

	new_size = len + offset;
	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > inode->i_size) {
		ret = inode_newsize_ok(inode, new_size);
		if (ret)
			return ret;
	} else {
		new_size = inode->i_size;
	}

	nova_dbgv("%s: inode %lu, offset %lld, count %lld, mode 0x%x\n",
			__func__, inode->i_ino,	offset, len, mode);

	NOVA_START_TIMING(fallocate_t, fallocate_time);
	inode_lock(inode);

	pi = nova_get_inode(sb, inode);
	if (!pi) {
		ret = -EACCES;
		goto out;
	}

	inode->i_mtime = inode->i_ctime = current_time(inode);
	time = current_time(inode).tv_sec;

	blocksize_mask = sb->s_blocksize - 1;
	start_blk = offset >> sb->s_blocksize_bits;
	blockoff = offset & blocksize_mask;
	num_blocks = (blockoff + len + blocksize_mask) >> sb->s_blocksize_bits;

	epoch_id = nova_get_epoch_id(sb);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;
	while (num_blocks > 0) {
		ent_blks = nova_check_existing_entry(sb, inode, num_blocks,
						start_blk, &entry, &entry_copy,
						1, epoch_id, &inplace, 1);

		entryc = (metadata_csum == 0) ? entry : &entry_copy;

		if (entry && inplace) {
			if (entryc->size < new_size) {
				/* Update existing entry */
				nova_memunlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);
				entry->size = new_size;
				nova_update_entry_csum(entry);
				nova_update_alter_entry(sb, entry);
				nova_memlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);
			}
			allocated = ent_blks;
			goto next;
		}

		/* Allocate zeroed blocks to fill hole */
		allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
				 ent_blks, ALLOC_INIT_ZERO, ANY_CPU,
				 ALLOC_FROM_HEAD);
		nova_dbgv("%s: alloc %d blocks @ %lu\n", __func__,
						allocated, blocknr);

		if (allocated <= 0) {
			nova_dbg("%s alloc %lu blocks failed!, %d\n",
						__func__, ent_blks, allocated);
			ret = allocated;
			goto out;
		}

		/* Handle hole fill write */
		nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
					start_blk, allocated, blocknr,
					time, new_size);

		ret = nova_append_file_write_entry(sb, pi, inode,
					&entry_data, &update);
		if (ret) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		entry = nova_get_block(sb, update.curr_entry);
		nova_reset_csum_parity_range(sb, sih, entry, start_blk,
					start_blk + allocated, 1, 0);

		update_log = true;
		if (begin_tail == 0)
			begin_tail = update.curr_entry;

		total_blocks += allocated;
next:
		num_blocks -= allocated;
		start_blk += allocated;
	}

	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));

	inode->i_blocks = sih->i_blocks;

	if (update_log) {
		sih->log_tail = update.tail;
		sih->alter_log_tail = update.alter_tail;

		nova_memunlock_inode(sb, pi, &irq_flags);
		nova_update_tail(pi, update.tail);
		if (metadata_csum)
			nova_update_alter_tail(pi, update.alter_tail);
		nova_memlock_inode(sb, pi, &irq_flags);

		/* Update file tree */
		ret = nova_reassign_file_tree(sb, sih, begin_tail);
		if (ret)
			goto out;

	}

	nova_dbgv("blocks: %lu, %lu\n", inode->i_blocks, sih->i_blocks);

	if (ret || (mode & FALLOC_FL_KEEP_SIZE)) {
		nova_memunlock_inode(sb, pi, &irq_flags);
		pi->i_flags |= cpu_to_le32(NOVA_EOFBLOCKS_FL);
		nova_memlock_inode(sb, pi, &irq_flags);
		sih->i_flags |= cpu_to_le32(NOVA_EOFBLOCKS_FL);
	}

	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > inode->i_size) {
		inode->i_size = new_size;
		sih->i_size = new_size;
	}

	nova_memunlock_inode(sb, pi, &irq_flags);
	nova_update_inode_checksum(pi);
	nova_update_alter_inode(sb, inode, pi);
	nova_memlock_inode(sb, pi, &irq_flags);

	sih->trans_id++;
out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
						begin_tail, update.tail);

	inode_unlock(inode);
	NOVA_END_TIMING(fallocate_t, fallocate_time);
	return ret;
}

static int nova_iomap_begin_nolock(struct inode *inode, loff_t offset,
	loff_t length, unsigned int flags, struct iomap *iomap)
{
	return nova_iomap_begin(inode, offset, length, flags, iomap, false);
}

static struct iomap_ops nova_iomap_ops_nolock = {
	.iomap_begin	= nova_iomap_begin_nolock,
	.iomap_end	= nova_iomap_end,
};

static ssize_t nova_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;
	INIT_TIMING(read_iter_time);

	if (!iov_iter_count(to))
		return 0;

	NOVA_START_TIMING(read_iter_t, read_iter_time);
	inode_lock_shared(inode);
	ret = dax_iomap_rw(iocb, to, &nova_iomap_ops_nolock);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	NOVA_END_TIMING(read_iter_t, read_iter_time);
	return ret;
}

static int nova_update_iter_csum_parity(struct super_block *sb,
	struct inode *inode, loff_t offset, size_t count)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long start_pgoff, end_pgoff;
	loff_t end;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	end = offset + count;

	start_pgoff = offset >> sb->s_blocksize_bits;
	end_pgoff = end >> sb->s_blocksize_bits;
	if (end & (nova_inode_blk_size(sih) - 1))
		end_pgoff++;

	nova_reset_csum_parity_range(sb, sih, NULL, start_pgoff,
			end_pgoff, 0, 0);

	return 0;
}

static ssize_t nova_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	loff_t offset;
	size_t count;
	ssize_t ret;
	INIT_TIMING(write_iter_time);

	NOVA_START_TIMING(write_iter_t, write_iter_time);
	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	ret = file_remove_privs(file);
	if (ret)
		goto out_unlock;

	ret = file_update_time(file);
	if (ret)
		goto out_unlock;

	count = iov_iter_count(from);
	offset = iocb->ki_pos;

	ret = dax_iomap_rw(iocb, from, &nova_iomap_ops_nolock);
	if (ret > 0 && iocb->ki_pos > i_size_read(inode)) {
		i_size_write(inode, iocb->ki_pos);
		sih->i_size = iocb->ki_pos;
		mark_inode_dirty(inode);
	}

	nova_update_iter_csum_parity(sb, inode, offset, count);

out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	NOVA_END_TIMING(write_iter_t, write_iter_time);
	return ret;
}
///^/// [yhc] Core function.
static ssize_t
do_dax_mapping_read(struct file *filp, char __user *buf,
	size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;		// [yhc] include/linux/fs.h
	struct super_block *sb = inode->i_sb;			// [yhc] include/linux/fs.h
	struct nova_inode_info *si = NOVA_I(inode);		// [yhc] state for inode.
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;			// [yhc] log.h
	struct nova_file_write_entry *entryc, entry_copy;
	pgoff_t index, end_index;				// [yhc] unsigned long type.
	unsigned long offset;
	loff_t isize, pos;					// [yhc] long offset: long long type.
	size_t copied = 0, error = 0;				// [yhc] unsigned int type.
	INIT_TIMING(memcpy_time);

	pos = *ppos;						// [yhc] target; current position.
	index = pos >> PAGE_SHIFT;				// [yhc] index of the page which the pos is located in.
	offset = pos & ~PAGE_MASK;				// [yhc] offset in that current position page.

	if (!access_ok(buf, len)) {
		error = -EFAULT;
		goto out;
	}

	isize = i_size_read(inode);		// [yhc] isize: file size in bytes.
	if (!isize)
		goto out;

	nova_dbgv("%s: inode %lu, offset %lld, count %lu, size %lld\n",
		__func__, inode->i_ino,	pos, len, isize);

	if (len > isize - pos)			// [yhc] Prevents the overflow.
		len = isize - pos;		// [yhc] len: length that should be read.

	if (len <= 0)
		goto out;			// [yhc] input error handling.

	entryc = (metadata_csum == 0) ? entry : &entry_copy;		// [yhc] setting-up the adress of a log entryc.

	end_index = (isize - 1) >> PAGE_SHIFT;	// [yhc] Calculates the last page # of the file.
	do {
		unsigned long nr, left;
		unsigned long nvmm;
		void *dax_mem = NULL;
		int zero = 0;

		/* nr is the maximum number of bytes to copy from this page */
		if (index >= end_index) {	// [yhc] Last iteration condition.
			if (index > end_index)
				goto out;
			nr = ((isize - 1) & ~PAGE_MASK) + 1;		// [yhc] I figured it out +_+! : Resolving the edge amount of last page.
			if (nr <= offset)
				goto out;
		}
//^//		// [yhc] Non-last iteration starts here.
		entry = nova_get_write_entry(sb, sih, index);		// [yhc] (1) radix tree look-up; (2) getting write entry of target page.
		if (unlikely(entry == NULL)) {
			nova_dbgv("Required extent not found: pgoff %lu, inode size %lld\n",
				index, isize);
			nr = PAGE_SIZE;					// [yhc] it seems to be 4KB!!
			zero = 1;
			goto memcpy;
		}

		if (metadata_csum == 0)
			entryc = entry;
		else if (!nova_verify_entry_csum(sb, entry, entryc))
			return -EIO;

		/* Find contiguous blocks */
		if (index < entryc->pgoff ||				// [yhc] entry??? ?????? ????????? index?????? ?????? ?????? ??????, ???????????? index page??? ??????x ??????,
			index - entryc->pgoff >= entryc->num_pages) {	// [yhc] error handling.
			nova_err(sb, "%s ERROR: %lu, entry pgoff %llu, num %u, blocknr %llu\n",
				__func__, index, entry->pgoff,
				entry->num_pages, entry->block >> PAGE_SHIFT);
			return -EINVAL;
		}
		if (entryc->reassigned == 0) {				// [yhc] 'nr' means number that should be read in this write entry.
			nr = (entryc->num_pages - (index - entryc->pgoff))
				* PAGE_SIZE;
		} else {
			nr = PAGE_SIZE;
		}

		nvmm = get_nvmm(sb, sih, entryc, index);		// [yhc] Resolve the address of target position excluding super_block.

	printk("READ: Reading pgoff(%lu ~ %lu), from datapage %lu \n", index, index + (nr / PAGE_SIZE) - 1, nvmm);
	printk("Reading %u pages from pgoff %llu \n", entry->num_pages, entry->pgoff);
	printk("index: %ld, nvmm(datapage): %ld \n", index, nvmm);
	//printk("Reading the time: %lld \n", inode->i_ctime.tv_sec);//???!

		dax_mem = nova_get_block(sb, (nvmm << PAGE_SHIFT));	// [yhc] Resolve the address of target block in PMEM.

	printk("dax_mem: %p \n", dax_mem);

memcpy:
		nr = nr - offset;					// [yhc] Calculates actual amount that should be read.
		if (nr > len - copied)
			nr = len - copied;				// [yhc]

		if ((!zero) && (data_csum > 0)) {
			if (nova_find_pgoff_in_vma(inode, index))
				goto skip_verify;

			if (!nova_verify_data_csum(sb, sih, nvmm, offset, nr)) {
				nova_err(sb, "%s: nova data checksum and recovery fail! inode %lu, offset %lu, entry pgoff %lu, %u pages, pgoff %lu\n",
					 __func__, inode->i_ino, offset,
					 entry->pgoff, entry->num_pages, index);
				error = -EIO;
				goto out;
			}
		}
skip_verify:
		NOVA_START_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (!zero)
			left = __copy_to_user(buf + copied,
						dax_mem + offset, nr);	// [yhc] (3) Actual operation: copy data from kernel space to user space.
		else
			left = __clear_user(buf + copied, nr);

		NOVA_END_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (left) {						// [yhc] If there are something left, it will be handled as an error.
			nova_dbg("%s ERROR!: bytes %lu, left %lu\n",
				__func__, nr, left);
			error = -EFAULT;
			goto out;
		}

		copied += (nr - left);			// [yhc] 1st assigned here at 1st loop; accumulates copied amount.
		offset += (nr - left);
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
	} while (copied < len);				// [yhc] Keep iterating until copied amount come to be same with length.

out:
	*ppos = pos + copied;
	if (filp)
		file_accessed(filp);			// [yhc] include/linux/fs.h: 2316.

	NOVA_STATS_ADD(read_bytes, copied);

	nova_dbgv("%s returned %zu\n", __func__, copied);
	return copied ? copied : error;
}

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * lock.
 */
static ssize_t nova_dax_file_read(struct file *filp, char __user *buf,
			    size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;
	ssize_t res;
	INIT_TIMING(dax_read_time);
printk("--DAX_READ-- \n");
	NOVA_START_TIMING(dax_read_t, dax_read_time);
	inode_lock_shared(inode);
	res = do_dax_mapping_read(filp, buf, len, ppos);			// [yhc] Mapping occurs here!
	inode_unlock_shared(inode);
	NOVA_END_TIMING(dax_read_t, dax_read_time);
	return res;
}

/*
 * Perform a COW write.   Must hold the inode lock before calling.
 */
static ssize_t do_nova_cow_file_write(struct file *filp,
	const char __user *buf,	size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode	*inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi, inode_copy;		// [yhc] pi: in-pmem nova_inode addr.
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;		// [yhc] update: transient DRAM structure that describes changes. 
	ssize_t	    written = 0;
	loff_t pos;
	size_t count, offset, copied;
	unsigned long start_blk, num_blocks;
	unsigned long total_blocks;
	unsigned long blocknr = 0;
	unsigned int data_bits;
	int allocated = 0;
	void *kmem;
	u64 file_size;
	size_t bytes;
	long status = 0;
	INIT_TIMING(cow_write_time);
	INIT_TIMING(memcpy_time);
	unsigned long step = 0;
	ssize_t ret;
	u64 begin_tail = 0;
	int try_inplace = 0;
	u64 epoch_id;
	u32 time;
	unsigned long irq_flags = 0;

	// DEDUP //
//unsigned char digest[40];
	unsigned char *fingerprint;
	unsigned char *dbuf;
	int i;
	struct fingerprint_lookup_data lookup_data;
	unsigned long nvmm;
	
	fingerprint = kmalloc(FINGERPRINT_SIZE, GFP_KERNEL);
	dbuf = kmalloc(DATABLOCK_SIZE, GFP_KERNEL);

	memset(fingerprint, 0, FINGERPRINT_SIZE);
	memset(dbuf, 0, DATABLOCK_SIZE);

//for (i = 0; i < FINGERPRINT_SIZE; i++)
//	printk("%d: %02X \n", i, fingerprint[i]);
	// DEDUP //

	if (len == 0)
		return 0;

	NOVA_START_TIMING(do_cow_write_t, cow_write_time);

	if (!access_ok(buf, len)) {	// [yhc] Ensures that the userspace application isn't asking the kernel to read from or write to kernel addresses.
		ret = -EFAULT;
		goto out;
	}
	pos = *ppos;
printk("starting offset: %lld \n", pos);	//[yc].
	if (filp->f_flags & O_APPEND)
		pos = i_size_read(inode);			// [yhc] i_size: file size in bytes.

	count = len;						// [yhc] 'count' saves the input length info.
printk("count : %ld \n",count);
printk("pos : %lld \n",pos);
printk("sih->pi_addr: %ld \n", sih->pi_addr);
	pi = nova_get_block(sb, sih->pi_addr);			// [yhc] pi_addr: address in PMEM of an inode.
printk("pi(lld): %lld \n", pi);
printk("pi(p)  : %p \n", pi);

	/* nova_inode tail pointer will be updated and we make sure all other
	 * inode fields are good before checksumming the whole structure
	 */
	if (nova_check_inode_integrity(sb, sih->ino, sih->pi_addr,
			sih->alter_pi_addr, &inode_copy, 0) < 0) {
		ret = -EIO;
		goto out;
	}
//^//   [yhc] Initializing essential local variables.
	offset = pos & (sb->s_blocksize - 1);					// [yhc] extracts offset of edge block. 's_blocksize': block size in bytes.
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;	// [yhc] calculates the necessary number of blocks.
	total_blocks = num_blocks;						// [yhc] 'total_blocks' saves the necessary number of blocks.
	start_blk = pos >> sb->s_blocksize_bits;				// [yhc] extracts the index of the starting block.
//^//
printk("total_blocks : %ld \n", total_blocks);
	if (nova_check_overlap_vmas(sb, sih, start_blk, num_blocks)) {
		nova_dbgv("COW write overlaps with vma: inode %lu, pgoff %lu, %lu blocks\n",
				inode->i_ino, start_blk, num_blocks);
		NOVA_STATS_ADD(cow_overlap_mmap, 1);
		try_inplace = 1;
		ret = -EACCES;
		goto out;
	}

	/* offset in the actual block size block */

	ret = file_remove_privs(filp);
	if (ret)
		goto out;

	inode->i_ctime = inode->i_mtime = current_time(inode);

printk("log head: %llu \n", pi->log_head);
printk("write ctime: %lld \n", inode->i_ctime.tv_sec);

	time = current_time(inode).tv_sec;

	nova_dbgv("%s: inode %lu, offset %lld, count %lu\n",
			__func__, inode->i_ino,	pos, count);

	epoch_id = nova_get_epoch_id(sb);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;
	while (num_blocks > 0) {		// [yhc] ???????????? block??? ?????????,
		printk("NOVAAA : helloo\n");	// [yhc] mnt??? dmesg??? ??????.
//test_hash("hello dedup nova", 16, digest);
//printk("***Dedup Hash: test_hash() -> %s \n", digest);	// hash testing.
printk("num_blocks : %ld\n", num_blocks);
		offset = pos & (nova_inode_blk_size(sih) - 1);
printk("writing - file offset %lld \n", pos);
		start_blk = pos >> sb->s_blocksize_bits;		//[yc] pos??? ???????????? ?????? ???????????????.

		/* don't zero-out the allocated blocks */
		allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
				 num_blocks, ALLOC_NO_INIT, ANY_CPU,
				 ALLOC_FROM_HEAD);

		// DEDUP //
		allocated = 1;		// [YhC] 1 Data-page per Write-entry.
		// DEDUP //
printk("blocknr: %d \n", blocknr);
printk("allocated: %d\n", allocated);
		nova_dbg_verbose("%s: alloc %d blocks @ %lu\n", __func__,
						allocated, blocknr);

		if (allocated <= 0) {
			nova_dbg("%s alloc blocks failed %d\n", __func__,
								allocated);
			ret = allocated;
			goto out;
		}

		step++;
		bytes = sb->s_blocksize * allocated - offset;		// [yhc] Subtracting edge block's edge. 
		if (bytes > count)
			bytes = count;					// [yhc] bytes: eventually saving length info.

		kmem = nova_get_block(inode->i_sb,			//[yc] kmem: actial address of starting block in PMEM.
			     nova_get_block_off(sb, blocknr, sih->i_blk_type));	// [yhc] Getting free blocks.

		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)) != 0)  {
			ret = nova_handle_head_tail_blocks(sb, inode, pos,
							   bytes, kmem);
			if (ret)
				goto out;
		}

		// DEDUP //
//		memcpy(dbuf, buf, DATABLOCK_SIZE);
		copy_from_user(dbuf, buf, DATABLOCK_SIZE);
		nova_dedup_fingerprint(dbuf, fingerprint);	// [YhC] Fingerprinting.

		for (i = 0; i < FINGERPRINT_SIZE; i++) {
			printk("%d: %02X \n", i, fingerprint[i]);
			lookup_data.fingerprint[i] = fingerprint[i];
		}


//		lookup_data.block_address = ;
//		nova_dedup_FACT_insert(sb, );


		// DEDUP //

		/* Now copy from user buf */
		//		nova_dbg("Write: %p\n", kmem);
printk("bytes: %ld\n", bytes);
		NOVA_START_TIMING(memcpy_w_nvmm_t, memcpy_time);
		nova_memunlock_range(sb, kmem + offset, bytes, &irq_flags);
		copied = bytes - memcpy_to_pmem_nocache(kmem + offset,		// [yhc] copied: saves remaining length.
						buf, bytes);			// [yhc] Actual memory copy occurs here.
		nova_memlock_range(sb, kmem + offset, bytes, &irq_flags);
		NOVA_END_TIMING(memcpy_w_nvmm_t, memcpy_time);
printk("after copied: %ld\n",copied);

		if (data_csum > 0 || data_parity > 0) {
			ret = nova_protect_file_data(sb, inode, pos, bytes,
							buf, blocknr, false);
			if (ret)
				goto out;
		}
										//[yc] i_size : file size in bytes.
		if (pos + copied > inode->i_size)				//[yc] just overwriting case.
			file_size = cpu_to_le64(pos + copied);
		else								//[yc] just overwriting case.
			file_size = cpu_to_le64(inode->i_size);

printk("WRITE: write %u(allocated) pages from %lu(start_blk) \n", allocated, start_blk);
		nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
					start_blk, allocated, blocknr, time,
					file_size);				// [yhc] Initializing 'file write entry'.
// DEDUP //
//nova_dedup_queue_init();	//[yc] initialize Dedup-Queue!
//printk("Dedup Queue init\n");
// DEDUP //
		ret = nova_append_file_write_entry(sb, pi, inode,
					&entry_data, &update);			// [yhc] Appending 'file write entry'.
		if (ret) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		nova_dbgv("Write: %p, %lu\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			buf += copied;
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0)
			break;

		if (begin_tail == 0)
			begin_tail = update.curr_entry;
	}

	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));

	nova_memunlock_inode(sb, pi, &irq_flags);
	nova_update_inode(sb, inode, pi, &update, 1);			// [yhc] Finally updating the very inode.
	nova_memlock_inode(sb, pi, &irq_flags);

	/* Free the overlap blocks after the write is committed */
	ret = nova_reassign_file_tree(sb, sih, begin_tail);
	if (ret)
		goto out;

	inode->i_blocks = sih->i_blocks;

	ret = written;
	NOVA_STATS_ADD(cow_write_breaks, step);
	nova_dbgv("blocks: %lu, %lu\n", inode->i_blocks, sih->i_blocks);

	*ppos = pos;
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		sih->i_size = pos;
	}

	sih->trans_id++;
out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
						begin_tail, update.tail);

	NOVA_END_TIMING(do_cow_write_t, cow_write_time);
	NOVA_STATS_ADD(cow_write_bytes, written);

	if (try_inplace)
		return do_nova_inplace_file_write(filp, buf, len, ppos);

	// DEDUP //
	kfree(fingerprint);
	// DEDUP //

	return ret;
}

/*
 * Acquire locks and perform COW write.
 */
ssize_t nova_cow_file_write(struct file *filp,
	const char __user *buf,	size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	int ret;
	INIT_TIMING(time);


	if (len == 0)
		return 0;

	NOVA_START_TIMING(cow_write_t, time);		// [yh] start timing.

	sb_start_write(inode->i_sb);
	inode_lock(inode);				// [yh] inode lock.

	ret = do_nova_cow_file_write(filp, buf, len, ppos);

	inode_unlock(inode);				// [yh] inode unlock.
	sb_end_write(inode->i_sb);

	NOVA_END_TIMING(cow_write_t, time);		// [yh] end timing.
	return ret;
}


static ssize_t nova_dax_file_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;

	if (test_opt(inode->i_sb, DATA_COW)) {
		printk("--COW--\n");		///////[yc].
		return nova_cow_file_write(filp, buf, len, ppos);
	}
	else {
		printk("--INPLACE--\n");	///////[yc].
		return nova_inplace_file_write(filp, buf, len, ppos);
	}
}

static ssize_t do_nova_dax_file_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;

	if (test_opt(inode->i_sb, DATA_COW))
		return do_nova_cow_file_write(filp, buf, len, ppos);
	else
		return do_nova_inplace_file_write(filp, buf, len, ppos);
}


static int nova_dax_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_mapping->host;

	file_accessed(file);

	vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;

	vma->vm_ops = &nova_dax_vm_ops;

	nova_insert_write_vma(vma);

	nova_dbg_mmap4k("[%s:%d] inode %lu, MMAP 4KPAGE vm_start(0x%lx), vm_end(0x%lx), vm pgoff %lu, %lu blocks, vm_flags(0x%lx), vm_page_prot(0x%lx)\n",
			__func__, __LINE__,
			inode->i_ino, vma->vm_start, vma->vm_end,
			vma->vm_pgoff,
			(vma->vm_end - vma->vm_start) >> PAGE_SHIFT,
			vma->vm_flags,
			pgprot_val(vma->vm_page_prot));

	return 0;
}

//[yc]
static ssize_t nova_dedup_inline(struct file *filp, size_t len)
{
	printk("#_# nova_dedup_inline was called!\n");
	return 100;
}

// [yhc] called by __vfs_read().
const struct file_operations nova_dax_file_operations = {
	// DEDUP //
	.dedup			= nova_dedup,
	// ----- //
	.llseek			= nova_llseek,
	.read			= nova_dax_file_read,
	.write			= nova_dax_file_write,
//[yc]
	.dedup_inline		= nova_dedup_inline,
	.read_iter		= nova_dax_read_iter,
	.write_iter		= nova_dax_write_iter,
	.mmap			= nova_dax_file_mmap,
	.mmap_supported_flags 	= MAP_SYNC,
	.open			= nova_open,
	.fsync			= nova_fsync,
	.flush			= nova_flush,
	.unlocked_ioctl		= nova_ioctl,
	.fallocate		= nova_fallocate,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= nova_compat_ioctl,
#endif
};


static ssize_t nova_wrap_rw_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *filp = iocb->ki_filp;
	struct inode *inode = filp->f_mapping->host;
	ssize_t ret = -EIO;
	ssize_t written = 0;
	unsigned long seg;
	unsigned long nr_segs = iter->nr_segs;
	const struct iovec *iv = iter->iov;
	INIT_TIMING(wrap_iter_time);

	NOVA_START_TIMING(wrap_iter_t, wrap_iter_time);

	nova_dbgv("%s %s: %lu segs\n", __func__,
			iov_iter_rw(iter) == READ ? "read" : "write",
			nr_segs);

	if (iov_iter_rw(iter) == WRITE)  {
		sb_start_write(inode->i_sb);
		inode_lock(inode);
	} else {
		inode_lock_shared(inode);
	}
		
	iv = iter->iov;
	for (seg = 0; seg < nr_segs; seg++) {
		if (iov_iter_rw(iter) == READ) {
			ret = do_dax_mapping_read(filp, iv->iov_base,
						  iv->iov_len, &iocb->ki_pos);
		} else if (iov_iter_rw(iter) == WRITE) {
			ret = do_nova_dax_file_write(filp, iv->iov_base,
						     iv->iov_len, &iocb->ki_pos);
		} else {
			BUG();
		}
		if (ret < 0)
			goto err;

		if (iter->count > iv->iov_len)
			iter->count -= iv->iov_len;
		else
			iter->count = 0;

		written += ret;
		iter->nr_segs--;
		iv++;
	}
	ret = written;
err:
	if (iov_iter_rw(iter) == WRITE)  {
		inode_unlock(inode);
		sb_end_write(inode->i_sb);
	} else {
		inode_unlock_shared(inode);
	}

	NOVA_END_TIMING(wrap_iter_t, wrap_iter_time);
	return ret;
}


/* Wrap read/write_iter for DP, CoW and WP */
const struct file_operations nova_wrap_file_operations = {
	// DEDUP //
	.dedup			= nova_dedup,
	// ----- //
	.llseek			= nova_llseek,
	.read			= nova_dax_file_read,
	.write			= nova_dax_file_write,
//[yc]
	.dedup_inline		= nova_dedup_inline,
	.read_iter		= nova_wrap_rw_iter,
	.write_iter		= nova_wrap_rw_iter,
	.mmap			= nova_dax_file_mmap,
	.get_unmapped_area = thp_get_unmapped_area,
	.open			= nova_open,
	.fsync			= nova_fsync,
	.flush			= nova_flush,
	.unlocked_ioctl		= nova_ioctl,
	.fallocate		= nova_fallocate,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= nova_compat_ioctl,
#endif
};

const struct inode_operations nova_file_inode_operations = {
	.setattr	= nova_notify_change,
	.getattr	= nova_getattr,
	.get_acl	= NULL,
};
/*** DEDUP ***/
//struct sdesc {
//    struct shash_desc shash;
//    char ctx[];
//};
/*
static struct sdesc *init_sdesc(struct crypto_shash *alg)
{
    struct sdesc *sdesc;
    int size;

    size = sizeof(struct shash_desc) + crypto_shash_descsize(alg);
    sdesc = kmalloc(size, GFP_KERNEL);
    if (!sdesc)
        return ERR_PTR(-ENOMEM);
    sdesc->shash.tfm = alg;
    sdesc->shash.flags = 0x0;
    return sdesc;
}

static int calc_hash(struct crypto_shash *alg,
             const unsigned char *data, unsigned int datalen,
             unsigned char *digest)
{
    struct sdesc *sdesc;
    int ret;

    sdesc = init_sdesc(alg);
    if (IS_ERR(sdesc)) {
        pr_info("can't alloc sdesc\n");
        return PTR_ERR(sdesc);
    }

    ret = crypto_shash_digest(&sdesc->shash, data, datalen, digest);
    kfree(sdesc);
    return ret;
}


//Test function, data is the data for hash, datalen is the data length, and digest is the sha1 result (is an out variable)

static int test_hash(const unsigned char *data, unsigned int datalen,
             unsigned char *digest)
{
    struct crypto_shash *alg;
    char *hash_alg_name = "sha1";
    int ret;

    alg = crypto_alloc_shash(hash_alg_name, 0, 0);
    if (IS_ERR(alg)) {
            pr_info("can't alloc alg %s\n", hash_alg_name);
            return PTR_ERR(alg);
    }
    ret = calc_hash(alg, data, datalen, digest);
    crypto_free_shash(alg);
    return ret;
}
*/
