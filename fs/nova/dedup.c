#include "dedup.h"

// struct rb_node temp_rb_node;
struct nova_dedup_queue nova_dedup_queue_head;

// Initialize Dedup-Queue
int nova_dedup_queue_init(void)
{
	INIT_LIST_HEAD(&nova_dedup_queue_head.list);
	nova_dedup_queue_head.write_entry_address = 0;				// initialize the contents of head as 0.
	return 0;
}

// Insert Write-entries to Dedup-Queue.
int nova_dedup_queue_push(u64 new_address, u64 target_inode_number)
{
	struct nova_dedup_queue *new_data;
	new_data = kmalloc(sizeof(struct nova_dedup_queue), GFP_KERNEL);
	list_add_tail(&new_data->list, &nova_dedup_queue_head.list);		// add to the list.
	new_data->write_entry_address = new_address;				// save the contents.
	new_data->target_inode_number = target_inode_number;
	printk("PUSH a write entry to the D-Queue: %llu, %llu\n", new_address, target_inode_number);
	return 0;
}

// Get next write entry to dedup.
u64 nova_dedup_queue_get_next_entry(u64 *target_inode_number)
{
	struct nova_dedup_queue *ptr;

//	if (nova_dedup_queue_head.list.next) {
	u64 ret = 0;
	if (!list_empty(&nova_dedup_queue_head.list)) {
		ptr = list_entry(nova_dedup_queue_head.list.next, struct nova_dedup_queue, list);	// ??! what means the "list"? -> a member
	printk("sizeof(struct list_head): %lu, sizeof(struct nova_dedup_queue): %lu\n", sizeof(struct list_head), sizeof(struct nova_dedup_queue));
		// assign values: call-by-value & call-by-reference
		ret = ptr->write_entry_address;
		*target_inode_number = ptr->target_inode_number;
		// delete from the list.
		list_del(nova_dedup_queue_head.list.next);	// dequeue the first element, [DEBUG] The problem>??
		kfree(ptr);	//
	printk("POP from queue: %llu, %llu \n", ret, *target_inode_number);
	}
	return ret;
}

// Initialize a Radix-tree leaf node.
//void nova_dedup_init_radix_tree_node(struct nova_dedup_radix_tree_node * node, loff_t entry_address)
//{
//	memset(node, 0, sizeof(struct nova_dedup_radix_tree_node));
//	node->dedup_table_entry = entry_address;
//}

//void nova_init_dedup_entry(struct dedup_node *entry) {
//	memset(entry, 0, sizeof(struct dedup_node));
//	entry->dedup_table_entry = 1;
//}


/*
Many user space methods cannot be used when writing kernel modules, such as Openssl.
But Linux itself provides a Crypto API for various encryption calculations of data.
Using this API, you can perform some encryption and signature operations in the kernel module.
The following is an example of SHA-1.
*/

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

/*
Test function, data is the data for hash, datalen is the data length, and digest is the sha1 result (is an out variable)
*/
//static int test_hash(const unsigned char *data, unsigned int datalen,
//             unsigned char *digest)

// Fingerprinting.
//int nova_dedup_fingerprint(char *datapage, char *ret_fingerprint)
int nova_dedup_fingerprint(unsigned char *datapage, unsigned char *ret_fingerprint)	// char -> unsigned char.
{
	struct crypto_shash *alg;
	char *hash_alg_name = "sha1";
	int ret;

	alg = crypto_alloc_shash(hash_alg_name, 0, 0);
	if (IS_ERR(alg)) {
		pr_info("can't alloc alg %s\n", hash_alg_name);
		return PTR_ERR(alg);
	}
//  ret = calc_hash(alg, data, datalen, digest);
	ret = calc_hash(alg, datapage, DATABLOCK_SIZE, ret_fingerprint);	// DEDUP //
	crypto_free_shash(alg);
	return ret;
}

// Return the number of new write-entries needed.
int nova_dedup_num_new_write_entry(bool *target, int num_pages)
{
	int ret = 1;
	int i;
	for (i = 0; i < num_pages - 1; i++) {
		if (target[i] != target[i+1])
			ret ++;
	}
	return ret;
}

// TODO update 'update-count', 'reference-count'
// TODO update 'dedup-flag' - inplace

// Update FACT table + dedup_flags int write entry.
int nova_dedup_update_FACT(struct super_block *sb, struct nova_inode_info_header * sih, u64 begin_tail)
{
	void *addr;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);
	unsigned long irq_flags = 0;

	while (curr_p && curr_p != sih->log_tail)
	{
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);
		if (curr_p == 0)
			break;

		addr = (void *) nova_get_block(sb, curr_p);
		entry = (struct nova_file_write_entry *) addr;

		// Update FACT corresponding to new write-entry.

		// Update new write-entry's 'dedup_flag'.
		nova_memunlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);
		entry->dedup_flag = 0;
		nova_update_entry_csum(entry);
		nova_update_alter_entry(sb, entry);
		nova_memlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);
	}
	return 0;
}

// Append a new Dedup-Table entry.
/*
ssize_t dedup_table_update(struct file *file, const void *buf, size_t count, loff_t *pos)
{
	mm_segment_t old_fs;
	ssize_t ret = -EINVAL;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_WRITE))
		return -EINVAL;
	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;

	if (count > MAX_RW_COUNT)
		count = MAX_RW_COUNT;
	file_start_write(file);

	if (file->f_op->write)
		ret = nova_inplace_file_write(file, buf, count, pos);

	if (ret > 0) {
		fsnotify_modify(file);
		add_wchar(current, ret);
	}
	inc_syscw(current);	/////////////////////////// ???
	file_end_write(file);

	set_fs(old_fs);
	return ret;
}
*/
///////////////////////////////////////////////////////////////////////////////

// Actual DEDUPLICATION-Service function //
int nova_dedup_test(struct file *filp)
{
	// for Radix-tree
//	struct nova_dedup_radix_tree_node temp;
//	void** temp2;		// Radix-tree look-up result.
//	struct dedup_node *temp3;
//	struct nova_dedup_radix_tree_node *temp3;

	// Super Block
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
//	struct nova_sb_info *sbi = NOVA_SB(sb);

	// for write-entry
	struct nova_file_write_entry *target_entry;	// Target write-entry to be deduplicated.
	struct inode *target_inode;			// inode of target write-entry.
	u64 entry_address;			// Address of target write-entry.
	u64 target_inode_number = 0;		// target inode number.

	struct nova_inode *target_pi, inode_copy;	// nova_inode of target inode.
	struct nova_inode_info *target_si;
	struct nova_inode_info_header *target_sih;

	char *buf;		// Read buffer space.
	char *fingerprint;	// FP result space.

	unsigned long left;
	pgoff_t index;		// page offset.
	int i, j, num_pages = 0;
	unsigned long nvmm;
	void *dax_mem = NULL;

	// For new write-entry.
	int num_new_entry = 0;
	//u64 new_entry_address[MAX_DATAPAGE_PER_WRITEENTRY];
	struct fingerprint_lookup_data *lookup_data;
	bool *duplicate_check;

	struct nova_inode_update update;
	struct nova_file_write_entry new_entry;	// new write-entry.
	u64 file_size;
	unsigned long blocknr = 0;
	unsigned long num_blocks = 0;
	unsigned long irq_flags = 0;
	u64 begin_tail = 0;
	u64 epoch_id;
	u32 time;

	// Others
	ssize_t ret;
	INIT_TIMING(dax_read_time);
	INIT_TIMING(cow_write_time);
	// ------------------------------------- //

printk("fs/nova/dedup.c\n");
printk("Initialize buffer, and fingerprint\n");

	buf = kmalloc(DATABLOCK_SIZE, GFP_KERNEL);		// 4KB-size buffer allocated.
	fingerprint = kmalloc(FINGERPRINT_SIZE, GFP_KERNEL);	// 20B-size fp space allocated.

	do {
		// Pop target write-entry.
		entry_address = nova_dedup_queue_get_next_entry(&target_inode_number);		// parameter added.
		num_new_entry = 0;
		//memset(new_entry_address, 0, MAX_DATAPAGE_PER_WRITEENTRY * 8);

		// target_inode_number should exist.
		if (target_inode_number < NOVA_NORMAL_INODE_START && target_inode_number != NOVA_ROOT_INO) {
			//
			printk("No entry! \n");
			continue;
		}

		if (entry_address != 0) {
			///// Should lock the file corresponding to write entry.
	
			target_inode = nova_iget(sb, target_inode_number);	//[yc] getting an inode which is corresponding to the target_inode_number.??!
			// Inode could have been deleted.
			if (target_inode == ERR_PTR(-ESTALE)) {			//[yc] error message.
				nova_info("%s: inode %llu does not exist.", __func__, target_inode_number);
				continue;
			}

			target_si = NOVA_I(target_inode);		// DRAM state for a target_inode.
			target_sih = &target_si->header;		// DRAM state for a target_inode header.
			target_pi = nova_get_inode(sb, target_inode);

		printk("inode number?: %llu \n", target_pi->nova_ino);

			// TODO cross check inode <-> write-entry //??!

			// Acquiring READ lock.
		//	INIT_TIMING(dax_read_time);
			NOVA_START_TIMING(dax_read_t, dax_read_time);
			inode_lock_shared(target_inode);

			// Read target write-entry.
			target_entry = nova_get_block(sb, entry_address);

		printk("sizeof(nova_file_write_entry) : %ld \n", sizeof(struct nova_file_write_entry));
		printk("write-entry block info: num_pages: %d, block: %lld, pgoff: %lld\n", target_entry->num_pages, target_entry->block, target_entry->pgoff);

			// Read 4096 Bytes from a write-entry.
			index = target_entry->pgoff;		// index: file offset at the beginning of this write.
			num_pages = target_entry->num_pages;	// number of pages.
			// allocating a kernel space for Fingerprint lookup.
			lookup_data = kmalloc(num_pages * sizeof(struct fingerprint_lookup_data), GFP_KERNEL);

			// iterate as much as # of data pages.
			for (i = 0; i < num_pages; i++) {

		printk("Data Page number %d ! \n", i + 1);

				// --- //
				memset(buf, 0, DATABLOCK_SIZE);
				memset(fingerprint, 0, FINGERPRINT_SIZE);

				// READ path like: Resolving the target address. //
				//nvmm = (unsigned long) (target_entry->block >> PAGE_SHIFT) + index - target_entry->pgoff;
				nvmm = get_nvmm(sb, target_sih, target_entry, index);
				dax_mem = nova_get_block (sb, (nvmm << PAGE_SHIFT));

				left = __copy_to_user(buf, dax_mem, DATABLOCK_SIZE);	// PMEM to DRAM.
				if (left) {
					nova_dbg("%s ERROR!: left %lu\n", __func__, left);
					return 0;
				}

		printk("Fingerprint Start \n");
				// Make Fingerprint
				nova_dedup_fingerprint(buf, fingerprint);

		printk("Fingerprint End \n");
				// Print Fp.
				for (j = 0; j < FINGERPRINT_SIZE; j++) {
					//printk("%d: %02X \n", j, fingerprint[j]);
					//printk("%08x", fingerprint[j]);
					lookup_data[i].fingerprint[j] = fingerprint[j];
				}
				//printk("\n");
				//printk("%c %c %c\n", buf[0], buf[1], buf[2]);
				index++;
			}

			duplicate_check = kmalloc(sizeof(bool) * num_pages, GFP_KERNEL);
			memset(duplicate_check, false, sizeof(bool) * num_pages);

			// TODO Lookup for duplicate datapages.
			// TODO add new 'DEDUP-table' entries.

			num_new_entry = nova_dedup_num_new_write_entry(duplicate_check, num_pages);

			// <DEBUG>
			for (i = 0; i < num_pages; i++) {
				for (j = 0; j < FINGERPRINT_SIZE; j++) {
					printk("%02X", lookup_data[i].fingerprint[j]);
				}
				printk("\n");
			}

			// READ Unlock.
			inode_unlock_shared(target_inode);
			NOVA_END_TIMING(dax_read_t, dax_read_time);
			
			// No more READ!!

			// TODO Write Lock
		//	INIT_TIMING(time);
			NOVA_START_TIMING(cow_write_t, cow_write_time);
			sb_start_write(target_inode->i_sb);
			inode_lock(target_inode);

			// TODO append new write-entries
			/* Should Know
				- how many entries are needed.
				- where is the starting block address.
				- dedup_flag should be set to 2.
				- num_pages are 1.
			*/

			if (nova_check_inode_integrity(sb, target_sih->ino, target_sih->pi_addr,
						target_sih->alter_pi_addr, &inode_copy, 0) < 0) {
				ret = -EIO;
				goto out;
			}
			// Set time variables.
			inode->i_ctime = inode->i_mtime = current_time(inode);
			time = current_time(inode).tv_sec;
			// === //
			epoch_id = nova_get_epoch_id(sb);
			update.tail = target_sih->log_tail;
			update.alter_tail = target_sih->alter_log_tail;

			// TODO update tail
			// Update Log-tail.
			nova_memunlock_inode(sb, target_pi, &irq_flags);
			nova_update_inode(sb, inode, target_pi, &update, 1);
			nova_memlock_inode(sb, target_pi, &irq_flags);

			// TODO update 'update-count', 'reference count'
			// TODO update 'dedup-flag' - inplace
			// Update FACT + dedup_flag.
			nova_dedup_update_FACT(sb, target_sih, begin_tail);

			// Update Radix Tree
			ret = nova_reassign_file_tree(sb, target_sih, begin_tail);
			if (ret)
				goto out;
			inode->i_blocks = target_sih->i_blocks;
out:
			if (ret < 0)
				printk("Clean up incomplete deduplication \n");

			// TODO Write Unlock
			inode_unlock(target_inode);
			sb_end_write(target_inode->i_sb);
			NOVA_END_TIMING(cow_write_t, cow_write_time);

			kfree(lookup_data);
			kfree(duplicate_check);
			//kfree(target_inode);
			iput(target_inode);	// ??!
			
			printk("DEDUP completed \n");
		}
		else {
			printk("no entry \n");
		}
	} while (0);

	// DEDUP-TABLE should be updated.

//	dedup_table_update(filp, buf, 32, &filp->f_pos);
//	printk("Dedup-Table update finished \n");
 
	// testing) assumes that Dedup-Queue-entry address is ~~~.
//	if (nova_dedup_queue_get_next_entry() != 0) { }
//	else printk("no entry!\n");

//	INIT_RADIX_TREE(&sbi->dedup_tree_fingerprint, GFP_KERNEL);	printk("fp Radix Tree Initialized \n");
//	INIT_RADIX_TREE(&sbi->dedup_tree_address, GFP_KERNEL);		printk("addr Radix Tree Initialized \n");

//	nova_init_dedup_entry(&temp);
//	nova_dedup_init_radix_tree_node(&temp, 1);	// testing) assumes that Dedup-table-entry address is '1'.

//	radix_tree_insert(&sbi->dedup_tree_fingerprint, 32, &temp);	printk("Radix-tree node inserted! \n");
//	temp2 = radix_tree_lookup_slot(&sbi->dedup_tree_fingerprint, 32);

//	if (temp2) {
//		printk("Looking up the Radix-tree, an entry was found...\n");
//		temp3 = radix_tree_deref_slot(temp2);
//		printk("dedup_table_entry: %lld \n", temp3->dedup_table_entry);
//	}

	kfree(buf);		// memory free for buffer space.
	kfree(fingerprint);	// memory free for fp space.
	return 0;
}

// TODO
// Implementation : How are we going to make the 'dedup table'? --> Static Table
// Design : How to search 'dedup table' for deduplication -> indexing.
// Design : How to search 'dedup table' for deletion -> indirect indexing.
// Implementation : How to gain file lock from 'write entry' -> nova_get_inode, nova_iget
