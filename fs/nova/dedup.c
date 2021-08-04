#include "dedup.h"
#include <linux/rbtree.h>

struct rb_node temp_rb_node;

struct nova_dedup_queue nova_dedup_queue_head;

// Initialize Dedup-Queue
int nova_dedup_queue_init(void)
{
	INIT_LIST_HEAD(&nova_dedup_queue_head.list);
	nova_dedup_queue_head.write_entry_address = 0;				// initialize the contents of head as 0.
	return 0;
}

// Insert Write-entries to Dedup-Queue.
int nova_dedup_queue_push(u64 new_address)
{
	struct nova_dedup_queue *new_data;
	new_data = kmalloc(sizeof(struct nova_dedup_queue), GFP_KERNEL);
	list_add_tail(&new_data->list, &nova_dedup_queue_head.list);		// add to the list.
	new_data->write_entry_address = new_address;				// save the contents.
	printk("PUSH a write entry to queue: %llu\n", new_address);
	return 0;
}

// Get next write entry to dedup.
u64 nova_dedup_queue_get_next_entry(void)
{
	struct nova_dedup_queue *ptr;

//	if (nova_dedup_queue_head.list.next) {
	u64 ret = 0;
	if (!list_empty(&nova_dedup_queue_head.list)) {
		ptr = list_entry(nova_dedup_queue_head.list.next, struct nova_dedup_queue, list);	// ??! what means the "list"? -> a member
//		printk("checking~ %llu \n", ptr->write_entry_address);
//		return 1;
//	}
//	else {
//		return 0;
		printk("sizeof(struct list_head): %lu, sizeof(struct nova_dedup_queue): %lu\n", sizeof(struct list_head), sizeof(struct nova_dedup_queue));
		ret = ptr->write_entry_address;
		list_del(nova_dedup_queue_head.list.next);	// dequeue the first element, [DEBUG] The problem>??
		kfree(ptr);	//
		printk("POP from queue: %llu \n", ret);
	}
	return ret;
}

// Initialize a Radix-tree leaf node.
void nova_dedup_init_radix_tree_node(struct nova_dedup_radix_tree_node * node, loff_t entry_address)
{
	memset(node, 0, sizeof(struct nova_dedup_radix_tree_node));
	node->dedup_table_entry = entry_address;
}

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
// DEDUP //
struct sdesc {
    struct shash_desc shash;
    char ctx[];
};

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
int nova_dedup_fingerprint(char *datapage, char *ret_fingerprint)
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
	struct nova_file_write_entry *target_entry;
	u64 entry_address;
	char *buf;
	char *fingerprint;
	unsigned long left;
	pgoff_t index;
	int i, j, data_page_number = 0;
	unsigned long nvmm;
	void *dax_mem = NULL;

	printk("fs/nova/dedup.c\n");

printk("Initialize buffer, and fingerprint\n");
	buf = kmalloc(DATABLOCK_SIZE, GFP_KERNEL);
	fingerprint = kmalloc(FINGERPRINT_SIZE, GFP_KERNEL);

	// Pop write Entry.
	entry_address = nova_dedup_queue_get_next_entry();

	if (entry_address != 0) {
		///// Should lock the file corresponding to write entry.

		// Read write_entry.
		target_entry = nova_get_block(sb, entry_address);
		printk("write-entry block info: num_pages: %d, block: %lld, pgoff: %lld\n", target_entry->num_pages, target_entry->block, target_entry->pgoff);
		// Read 4096 Bytes from a write-entry.
		index = target_entry->pgoff;
		data_page_number = target_entry->num_pages;

		for (i = 0; i < data_page_number; i++) {
	printk("Data Page number %d ! \n", i + 1);
			nvmm = (unsigned long) (target_entry->block >> PAGE_SHIFT) + index - target_entry->pgoff;
			dax_mem = nova_get_block (sb, (nvmm << PAGE_SHIFT));
			memset(buf, 0, DATABLOCK_SIZE);
			memset(fingerprint, 0, FINGERPRINT_SIZE);
			left = __copy_to_user(buf, dax_mem, DATABLOCK_SIZE);	// PMEM to DRAM.
			if (left) {
				nova_dbg("%s ERROR!: left %lu\n", __func__, left);
				return 0;
			}

			// Make Fingerprint
			nova_dedup_fingerprint(buf, fingerprint);

			// Print Fp.
			for (i = 0; i < FINGERPRINT_SIZE; i++)
				printk("%08x", fingerprint[i]);
			printk("\n");

	//		printk("%c %c %c\n", buf[0], buf[1], buf[2]);

			index++;
		}
	}
	else printk("no entry");

	// DEDUP-TABLE should be updated.

//	dedup_table_update(filp, buf, 32, &filp->f_pos);
	printk("Dedup-Table update finished \n");
 
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

	kfree(buf);
//kfree(fingerprint);
	return 0;
}
