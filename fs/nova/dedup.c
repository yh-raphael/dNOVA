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
	printk("PUSH a write entry to the D-Queue => Write-entry address: %llu, inode number: %llu\n", new_address, target_inode_number);
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
	printk("POP from queue => Write-entry address: %llu, inode number: %llu \n", ret, *target_inode_number);
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

// Return the number of new write-entries needed.	???!!
int nova_dedup_num_new_write_entry(short *target, int num_pages)
{
	int ret = 1;		// Separate data pages.
	int invalid_count = 0;	// Invalid data pages.

	int i;
	for (i = 0; i < num_pages - 1; i++) {
		if (target[i] != target[i+1]) {
			if (target[i] == 2)
				invalid_count ++;
			else if (i == num_pages - 2 && target[i + 1] == 2)
				invalid_count ++;
			ret ++;
		}
	}

	if (ret == 1) {
		if (target[0] == 2)
			ret = 0;
	}
	return ret - invalid_count;
}

int nova_dedup_crosscheck(struct nova_file_write_entry *entry,
				struct nova_inode_info_header *sih, unsigned long pgoff)
{
	struct nova_file_write_entry *referenced_entry;
	void **pentry;

	pentry = radix_tree_lookup_slot(&sih->tree, pgoff);
	if (!pentry)		// if there is no entry.
		return 0;
	referenced_entry = radix_tree_deref_slot(pentry);

	if (referenced_entry == entry)
		return 1;
	else {
		printk("Invalid Datapage detected! \n");
		return 0;
	}
}

int nova_dedup_reassign_file_tree(struct super_block *sb, struct nova_inode_info_header *sih, u64 begin_tail)
{
	void *addr;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	while (curr_p && curr_p != sih->log_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) { 
			nova_err(sb, "%s: File inode %lu log is NULL!\n",
					__func__, sih->ino);
			return -EINVAL;
		}    

		addr = (void *) nova_get_block(sb, curr_p);
		entry = (struct nova_file_write_entry *) addr;

		if (metadata_csum == 0)
			entryc = entry;
		else if (!nova_verify_entry_csum(sb, entry, entryc))
			return -EIO;

		if (nova_get_entry_type(entryc) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n",
					__func__, nova_get_entry_type(entry));
			curr_p += entry_size;
			continue;
		}    

		nova_assign_write_entry(sb, sih, entry, entryc, false);
		curr_p += entry_size;
	}

	return 0;
}

int nova_dedup_invalidate_target_entry(struct super_block *sb, 
		struct nova_inode_info_header *sih, struct nova_file_write_entry *target_entry)
{
	unsigned long start_pgoff = target_entry->pgoff;
	unsigned int num = target_entry->num_pages;
	unsigned int num_free = 0;
	unsigned long curr_pgoff;
	unsigned long start_blocknr = (target_entry->block) >> PAGE_SHIFT;
	unsigned long curr_blocknr;
	// Free Data pages that are duplicate
	// Invalidate Target Entry
	// radix_tree_replace_slot

	int i;
	int ret = 0;
	INIT_TIMING(assign_time);

	NOVA_START_TIMING(assign_t, assign_time);
	for (i = 0; i < num; i++) { 
		curr_pgoff = start_pgoff + i;
		curr_blocknr = start_blocknr + i;

		// duplicate: Free (not inside dedup table)
		if (nova_dedup_is_duplicate(sb, curr_blocknr, true) == 2)
			nova_free_old_entry(sb, sih,target_entry,
					curr_pgoff, 1, false, target_entry->epoch_id);
		// unique: Don't Free
		else
			nova_invalidate_write_entry(sb, target_entry, 1, 1);
	} 

	nova_invalidate_write_entry(sb, target_entry, 1, 0);
out:
	NOVA_END_TIMING(assign_t, assign_time);
	return ret;
}


// === Functions for FACT === //
int nova_dedup_FACT_update_count(struct super_block *sb, u64 index)
{
	u32 count = 0;
	u8 compare = (1 << 4) - 1;

	struct fact_entry* target_entry;
	unsigned long irq_flags = 0;

	// reslove actual index.
	u64 target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry *) nova_get_block(sb, target_index);

	target_index = target_entry->delete_target;
	// reslove count of actual index.
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + target_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry *) nova_get_block(sb, target_index);
	count = target_entry->count;

	printk("UpdateCount : %d \n", count);

	// If UpdateCount > 0.
	if (compare & count) {
		// decrease updateCount 1.
		// increase referenceCount 1.
		count += 15;
		if (count > ((1UL << 32) - 1)) {	// UL: Unsigned Long
			printk("ERROR: overflow! \n");
			return 1;
		}
		// ReferenceCount, updateCount - Atomic update.
		nova_memunlock_range(sb, target_entry, NOVA_FACT_ENTRY_SIZE, &irq_flags);
		PERSISTENT_BARRIER();	///////////////////////////////// Atomic ??!
		target_entry->count = count;
		nova_memlock_range(sb, target_entry, NOVA_FACT_ENTRY_SIZE, &irq_flags);
	}
	return 0;
}

// TODO update 'update-count', 'reference-count'
// TODO update 'dedup-flag' - inplace

// Find FACT-entry using index(of FACT)
int nova_dedup_FACT_read(struct super_block *sb, u64 index)
{
//	int j;
	int r_count, u_count;
	struct fact_entry *target;

	u64 target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
	target = (struct fact_entry *) nova_get_block(sb, target_index);

//	printk("is it 1?: %d \n", target->count);	///// ??!
//	for (j = 0; j < FINGERPRINT_SIZE; j++) {
//		printk("%02X", target->fingerprint[j]);
//	}
//	printk("\n");

	r_count = target->count;
	u_count = target->count;
	r_count >>= 4;	// x...xyyyy -> x...x
	u_count &= 15;	// 15 means 1111b

	printk("FACT READ completed, referenceCount: %d, updateCount: %d\n", r_count, u_count);
	return 0;
}

// Is FACT-entry empty?
int nova_dedup_is_empty(struct fact_entry target)
{
	if (target.count == 0)
		return 1;
	return 0;
}

// TODO insert delete entries too.

// Insert new FACT-entry.
int nova_dedup_FACT_insert(struct super_block *sb, struct fingerprint_lookup_data *lookup)
{
	unsigned long irq_flags = 0;
	struct fact_entry te;		// target entry.
	struct fact_entry *pmem_te;	// PMEM target entry.
	u64 index = 0;
	u64 target_index;
	int ret = 0;

	index = lookup->fingerprint[0];
	index = index << 8 | lookup->fingerprint[1];		// total 24 bit is composed.
	index = index << 8 | lookup->fingerprint[2];				// Why assigning repeatedly ??? ///////////////

	// Read entries until it finds a match or finds an empty slot.
	do {
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
		pmem_te = (struct fact_entry *) nova_get_block(sb, target_index);
		__copy_to_user(&te, pmem_te, sizeof(struct fact_entry));	// What is the &te: not assigned!!! /////////////

		// duplicate case.
		if (strncmp(te.fingerprint, lookup->fingerprint, FINGERPRINT_SIZE) == 0) {
			ret = 1;
			break;
		}
		// unique case.
		if (nova_dedup_is_empty(te)) {
			ret = 0;
			break;
		}

		// TODO add pointer to the entry and add a new entry at the end of fact table.

	} while (0);

	// duplicate Data-page detected.
	if (ret) {
		if ((te.count & ((1 << 4) - 1)) == ((1 << 4) - 1)) {			// Is this calculation right??!!! /////////////////
			printk("ERROR: more than 16 updates to this entry! \n");
			return -1;
		}
		te.count ++;
		printk("Duplicate page detected, count is %d \n", te.count);
	}
	// new entry should be written.
	else {
		strncpy(te.fingerprint, lookup->fingerprint, FINGERPRINT_SIZE);
		te.block_address = lookup->block_address;
		te.count = 1;	// set up the UpdateCount field.
		te.next = 0;
	}

	// Copy target-entry to PMEM.
	nova_memunlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
	memcpy_to_pmem_nocache(pmem_te, &te, NOVA_FACT_ENTRY_SIZE - 4);	// Do not WRITE on 'delete' field.
	nova_memlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);

	// update lookup data.
	lookup->index = index;
	lookup->block_address = te.block_address;

	// Add FACT entry for delete.
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + te.block_address * NOVA_FACT_ENTRY_SIZE;
	pmem_te = (struct fact_entry *) nova_get_block(sb, target_index);
	__copy_to_user(&te, pmem_te, sizeof(struct fact_entry));

	te.delete_target = index;

	nova_memunlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
	memcpy_to_pmem_nocache(pmem_te, &te, NOVA_FACT_ENTRY_SIZE);
	nova_memlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);

	return ret;
}

// Update FACT table + dedup_flags in write entry.
int nova_dedup_entry_update(struct super_block *sb, struct nova_inode_info_header * sih, u64 begin_tail)
{
	void *addr;
	struct nova_file_write_entry *entry;
	//struct nova_file_write_entry *entryc, entry_copy;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);
	unsigned long irq_flags = 0;

	unsigned long curr_index;
	unsigned long start_index;
	unsigned int num = 0;
	int i;

	while (curr_p && curr_p != sih->log_tail)
	{
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);
		if (curr_p == 0)
			break;

		addr = (void *) nova_get_block(sb, curr_p);
		entry = (struct nova_file_write_entry *) addr;

		// Update FACT corresponding to new write-entry.
		// 1. Know data page address (blocknr)
		// 2. Know the index of that datapage in FACT
		// 3. Call nova_dedup_FACT_update

		num = entry->num_pages;
		start_index = entry->block >> PAGE_SHIFT;
		for (i = 0; i < num; i++)
		{
			curr_index = start_index + i;
			nova_dedup_FACT_update_count(sb, curr_index);
		}

		// Update new write-entry's 'dedup_flag'.
		nova_memunlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);
		entry->dedup_flag = 0;
		nova_update_entry_csum(entry);
		nova_update_alter_entry(sb, entry);
		nova_memlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);

		curr_p += entry_size;
	}
	return 0;
}

// Check whether target block has multiple ReferenceCount.
// Return value - 0: it's not okay to delete, 1: it's okay to delete
int nova_dedup_is_duplicate(struct super_block *sb, unsigned long blocknr, bool check)
{
	unsigned long irq_flags = 0;
	struct fact_entry te;		// target entry.
	struct fact_entry* pmem_te;	// PMEM target entry.

	u64 index = 0;
	u64 target_index;
	int ret = 0;

	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + blocknr * NOVA_FACT_ENTRY_SIZE;
	pmem_te = (struct fact_entry *) nova_get_block(sb, target_index);
	__copy_to_user(&te, pmem_te, sizeof(struct fact_entry));

	index = te.delete_target;

	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
	pmem_te = (struct fact_entry *) nova_get_block(sb, target_index);
	__copy_to_user(&te, pmem_te, sizeof(struct fact_entry));

	ret = te.count >> 4;

	if (ret <= 0) {
		printk("ERROR: Block is not in FACT! \n");
		return 2;
	}
	else {
		if (!check) {
//			te.count -= 16;	// MAYBE bug!@_@

			nova_memunlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
			memcpy_to_pmem_nocache(pmem_te, &te, NOVA_FACT_ENTRY_SIZE - 4);	// Do not WRITE on 'delete' field.
			nova_memlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);

			if (ret == 1)
				return 1;	// referenceCount == 1.
			else
				return 0;	// referenceCount >= 2.
		}
	}

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

	// Read Super Block. //
	struct address_space *mapping = filp->f_mapping;
//	struct inode *inode = mapping->host;
//	struct super_block *sb = inode->i_sb;
	struct inode *garbage_inode = mapping->host;
	struct super_block *sb = garbage_inode->i_sb;
//	struct nova_sb_info *sbi = NOVA_SB(sb);

	// For READ phase. //
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

	// For WRITE phase. //
	int num_new_entry = 0;
	int start, end;

	//u64 new_entry_address[MAX_DATAPAGE_PER_WRITEENTRY];
	struct fingerprint_lookup_data *lookup_data;
	struct nova_inode_update update;
//	struct nova_file_write_entry new_entry;		// new write-entry.
	struct nova_file_write_entry entry_data;	// new write-entry.
	short *duplicate_check;
	u64 file_size;
	unsigned long original_start_blk, start_blk;
	unsigned long blocknr = 0;
	unsigned long num_blocks = 0;
	unsigned long irq_flags = 0;
	u64 begin_tail = 0;
	u64 epoch_id;
	u32 time;
	u32 valid_page_num = 0;

	// Others
	ssize_t ret = 0;
	//INIT_TIMING(dax_read_time);
	//INIT_TIMING(cow_write_time);
	// ------------------------------------- //

printk("fs/nova/dedup.c\n");
printk("Initialize buffer, and fingerprint\n");

	// kmalloc buf, Fingerprint.
	buf = kmalloc(DATABLOCK_SIZE, GFP_KERNEL);		// 4KB-size buffer allocated.
	fingerprint = kmalloc(FINGERPRINT_SIZE, GFP_KERNEL);	// 20B-size fp space allocated.

	do {
		printk("---------- DEDUP start! ---------- \n");
		// Pop target write-entry.
		entry_address = nova_dedup_queue_get_next_entry(&target_inode_number);		// parameter added.
		//memset(new_entry_address, 0, MAX_DATAPAGE_PER_WRITEENTRY * 8);

		// target_inode_number should exist.
		if (target_inode_number < NOVA_NORMAL_INODE_START && target_inode_number != NOVA_ROOT_INO) {
			//
			printk("No entry! \n");
			continue;
		}

		// -------------------- READ phase - read the target inode. -------------------- //
		target_inode = nova_iget(sb, target_inode_number);	//[yc] getting an inode which is corresponding to the target_inode_number.??!
		// inode could have been deleted.
		if (target_inode == ERR_PTR(-ESTALE)) {
			nova_info("%s: inode %llu does not exist.", __func__, target_inode_number);
			continue;
		}

		// Dedup-Queue has contents.
		if (entry_address != 0) {
			///// Should lock the file corresponding to write entry.
			ret = 0;
			// Initialize variables.
			num_new_entry = 0;
			valid_page_num = 0;
			original_start_blk = 0;
			begin_tail = 0;
			irq_flags = 0;

			target_si = NOVA_I(target_inode);		// DRAM state for a target_inode.
			target_sih = &target_si->header;		// DRAM state for a target_inode header.
			target_pi = nova_get_inode(sb, target_inode);

		printk("inode number?: %llu \n", target_pi->nova_ino);

			// TODO cross check inode <-> write-entry //??!

			// -------------------- Lock acquiring -------------------- //
			sb_start_write(target_inode->i_sb);
			inode_lock(target_inode);

			//INIT_TIMING(dax_read_time);
			//NOVA_START_TIMING(dax_read_t, dax_read_time);
			//inode_lock_shared(target_inode);

			// Read target Write-entry.
			target_entry = nova_get_block(sb, entry_address);
			original_start_blk = target_entry->pgoff;

		printk("sizeof(nova_file_write_entry) : %ld \n", sizeof(struct nova_file_write_entry));

			// Read 4096 Bytes from a write-entry.
			index = target_entry->pgoff;		// index: file offset at the beginning of this write.
			num_pages = target_entry->num_pages;	// number of pages.
			// allocating a kernel space for Fingerprint lookup.
			lookup_data = kmalloc(num_pages * sizeof(struct fingerprint_lookup_data), GFP_KERNEL);
			duplicate_check = kmalloc(sizeof(short) * num_pages, GFP_KERNEL);
			memset(duplicate_check, false, sizeof(short) * num_pages);

		printk("write-entry block info: num_pages: %d, block: %lld, pgoff: %lld\n", target_entry->num_pages, target_entry->block, target_entry->pgoff);

			// READ each Data-page from target Write-entry.
			// iterate as much as # of data pages.
			for (i = 0; i < num_pages; i++) {

		printk("Data Page number %d ! \n", i + 1);

				if (nova_dedup_crosscheck(target_entry, target_sih, index) == 0) {
					duplicate_check[i] = 2;		// Data-page i is invalid, target Write-entry does not point to it!
					index ++;
					continue;
				}

				valid_page_num ++;
				memset(buf, 0, DATABLOCK_SIZE);
				memset(fingerprint, 0, FINGERPRINT_SIZE);

				// READ path like: Resolving the target address. //
				//nvmm = (unsigned long) (target_entry->block >> PAGE_SHIFT) + index - target_entry->pgoff;
				nvmm = get_nvmm(sb, target_sih, target_entry, index);
				dax_mem = nova_get_block (sb, (nvmm << PAGE_SHIFT));

				left = __copy_to_user(buf, dax_mem, DATABLOCK_SIZE);	// PMEM to DRAM: Read Data-page.
				if (left) {
					nova_dbg("%s ERROR!: left %lu\n", __func__, left);
					return 0;
				}

		printk("Fingerprint Start \n");
				// Fingerprint each Data-page.
				nova_dedup_fingerprint(buf, fingerprint);
		printk("Fingerprint End \n");

				// Print Fingerprint.
				for (j = 0; j < FINGERPRINT_SIZE; j++) {
					printk("%d: %02X \n", j, fingerprint[j]);
					//printk("%08x", fingerprint[j]);
					lookup_data[i].fingerprint[j] = fingerprint[j];
				}
				//printk("\n");
				//printk("%c %c %c\n", buf[0], buf[1], buf[2]);
				lookup_data[i].block_address = nvmm;	// *** Assigining the block_address! *** //
				index++;
			}

			// Lookup & Add to the FACT.
			for (i = 0; i < num_pages; i++)
				if (duplicate_check[i] != 2)
					duplicate_check[i] = nova_dedup_FACT_insert(sb, &lookup_data[i]);

			// Test
			for (i = 0; i < num_pages; i++)
				if (duplicate_check[i] != 2)
					nova_dedup_FACT_read(sb, lookup_data[i].index);


			// Get the number of new Write-entries needed to be appended.
			num_new_entry = nova_dedup_num_new_write_entry(duplicate_check, num_pages);
			if (num_new_entry == 0) {
				printk("No valied Data-page \n");
				goto out;
			}

			// TODO Lookup for duplicate datapages.
//			for (i = 0; i < num_pages; i++)
//				nova_dedup_FACT_insert(sb, &lookup_data[i]);

//			for (i = 0; i < num_pages; i++)
//				nova_dedup_FACT_read(sb, lookup_data[i].index);

			// TODO add new 'FACT' entries.

			// Construct lookup_data.


			// READ Unlock.
			//inode_unlock_shared(target_inode);
			//NOVA_END_TIMING(dax_read_t, dax_read_time);
			
			// No more READ!!

			// TODO Write Lock
			//INIT_TIMING(time);
			//NOVA_START_TIMING(cow_write_t, cow_write_time);
			//sb_start_write(target_inode->i_sb);
			//inode_lock(target_inode);

			// TODO append new write-entries
			/* Should Know
				- how many entries are needed.
				- where is the starting block address.
				- dedup_flag should be set to 2.
				- num_pages are 1.
			*/

			// -------------------- WRITE phase. -------------------- //
			if (nova_check_inode_integrity(sb, target_sih->ino, target_sih->pi_addr,
						target_sih->alter_pi_addr, &inode_copy, 0) < 0) {
				ret = -EIO;
				goto out;
			}

			// Set time variables.
			target_inode->i_ctime = current_time(target_inode);
			time = current_time(target_inode).tv_sec;
			// === //
			epoch_id = nova_get_epoch_id(sb);
			update.tail = target_sih->log_tail;
			update.alter_tail = target_sih->alter_log_tail;


			for (i = 0; i < num_pages; i++)
			{
				start = i;
				end = i;

				// unique case!
				if (duplicate_check[i] == 0) {
					for (j = i; j < num_pages - 1; j++) {
						if (duplicate_check[j + 1] == 0)
							end = j + 1;	// unique
						else
							break;		// duplicate or invalid
					}
					// start ~ end is unique.
					i = j;
				}
				// invalid case!
				else if (duplicate_check[i] == 2)
					continue;

				// start ~ end should go into Data-pages.
				// start_blk - block offset inside the file (0, 1 ...) = offset of 'start'
				start_blk = original_start_blk + start;
				// num_blocks - # of blocks = (end - start + 1)
				num_blocks = (end - start) + 1;

				// blocknr = block starting address(Data-page) (2341413 something like this) = blocknr of start.
				blocknr = lookup_data[start].block_address;

				// file_size - size of file after write (does not change)
				file_size = cpu_to_le64(target_inode->i_size);
				if (duplicate_check[start] == 1) {
					printk("file size shrink \n");
					file_size -= DATABLOCK_SIZE;
				}

				// vanila NOVA WRITE path like //
				printk("NEW WRITE ENTRY: start pgoff: %lu, number of pages: %lu \n", start_blk, num_blocks);

				nova_init_file_write_entry(sb, target_sih, &entry_data, epoch_id,
							start_blk, num_blocks, blocknr, time, file_size);
				entry_data.dedup_flag = 2;	// flag is set to 2.
				ret = nova_append_file_write_entry(sb, target_pi, target_inode, &entry_data, &update);
				if (ret) {
					nova_dbg("%s: append inode entry failed\n", __func__);
					ret = -ENOSPC;
					//goto out;
				}
				if (begin_tail == 0)
					begin_tail = update.curr_entry;
				valid_page_num -= num_blocks;
			}
			// Non-appended pages exist.
			if (valid_page_num != 0) {
				printk("Datapage assign error! %d left \n", valid_page_num);
				goto out;
			}

			// Update Log-tail pointer.
			nova_memunlock_inode(sb, target_pi, &irq_flags);
			nova_update_inode(sb, target_inode, target_pi, &update, 1);
			nova_memlock_inode(sb, target_pi, &irq_flags);

			// TODO update 'update-count', 'reference count'
			// TODO update 'dedup-flag' - inplace
			// Update FACT + dedup_flag.
			nova_dedup_entry_update(sb, target_sih, begin_tail);

			// --- WRITE path like --- //
			// Test(updated count)
			for (i = 0; i < num_pages; i++) {
				if (duplicate_check[i] != 2) {
					nova_dedup_FACT_read(sb, lookup_data[i].index);
				}
			}

			// For testing.
//			for (i = 0; i < num_pages; i++) {
//				printk("Data-page number: %d \n", i + 1);
//				for (j = 0; j < FINGERPRINT_SIZE; j++) {
//					printk("%02X", lookup_data[i].fingerprint[j]);
//				}
//				printk("\n");
//			}
			// Update Radix Tree
			ret = nova_dedup_reassign_file_tree(sb, target_sih, begin_tail);	// ???!! 엔트리 정보를 바꿔준는 것일듯! 
			if (ret)
				goto out;

			ret = nova_dedup_invalidate_target_entry(sb, target_sih, target_entry);
			if (ret)
				goto out;

			target_inode->i_blocks = target_sih->i_blocks;
out:
			if (ret < 0)
				printk("Clean up incomplete deduplication \n");

			// -------------------- Unlock -------------------- //
			inode_unlock(target_inode);
			sb_end_write(target_inode->i_sb);
			//NOVA_END_TIMING(cow_write_t, cow_write_time);

			kfree(lookup_data);
			kfree(duplicate_check);
			//kfree(target_inode);
			iput(target_inode);	// ??!	
		}
		else {
			printk("no entry \n");
		}
		printk("---------- DEDUP completed ----------\n");
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
