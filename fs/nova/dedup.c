#include "dedup.h"

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
		ptr = list_entry(nova_dedup_queue_head.list.next, struct nova_dedup_queue, list);	// ??! what means the "list"?
//		printk("checking~ %llu \n", ptr->write_entry_address);
//		return 1;
//	}
//	else {
//		return 0;
		printk("sizeof(struct list_head): %lu, sizeof(struct nova_dedup_queue): %lu\n", sizeof(struct list_head), sizeof(struct nova_dedup_queue));
		ret = ptr->write_entry_address;
		list_del(nova_dedup_queue_head.list.next);	// The problem>??
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

// Fingerprinting.
void nova_fingerprint(char *datapage, char *ret_fingerprint)
{

}

int nova_dedup_test(struct file *filp)
{
	struct nova_dedup_radix_tree_node temp;
	void** temp2;
//	struct dedup_node *temp3;
	struct nova_dedup_radix_tree_node *temp3;

	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);

	printk("fs/nova/dedup.c\n");

	INIT_RADIX_TREE(&sbi->dedup_tree_fingerprint, GFP_KERNEL);	printk("fp Radix Tree Initialized \n");
	INIT_RADIX_TREE(&sbi->dedup_tree_address, GFP_KERNEL);		printk("addr Radix Tree Initialized \n");
 
//	if (nova_dedup_queue_get_next_entry() == 1) {	// testing) assumes that Dedup-Queue-entry address is ~~~.
//	}
//	else {
//		printk("no entry!\n");
//	}

	// testing) assumes that Dedup-Queue-entry address is ~~~.
	if (nova_dedup_queue_get_next_entry() != 0) { }
	else printk("no entry!\n");
	if (nova_dedup_queue_get_next_entry() != 0) { }
	else printk("no entry!\n");
	if (nova_dedup_queue_get_next_entry() != 0) { }
	else printk("no entry!\n");
	if (nova_dedup_queue_get_next_entry() != 0) { }
	else printk("no entry!\n");

//	nova_init_dedup_entry(&temp);
	nova_dedup_init_radix_tree_node(&temp, 1);	// testing) assumes that Dedup-table-entry address is '1'.

	radix_tree_insert(&sbi->dedup_tree_fingerprint, 32, &temp);	printk("Inserted! \n");
	temp2 = radix_tree_lookup_slot(&sbi->dedup_tree_fingerprint, 32);

	if (temp2) {
		printk("Found Entry\n");
		temp3 = radix_tree_deref_slot(temp2);
		printk("%lld \n", temp3->dedup_table_entry);
	}

	return 0;
}
