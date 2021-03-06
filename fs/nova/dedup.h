#ifndef __DEDUP_H__
#define __DEDUP_H__

#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/falloc.h>
#include <linux/sched/xacct.h>
#include <asm/mman.h>
#include <asm/uaccess.h>
#include <linux/radix-tree.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>

#include "nova.h"
#include "inode.h"

/*** FingerPrint ***/
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/crypto.h>


#define DATABLOCK_SIZE		4096
#define FINGERPRINT_SIZE	20

#define MAX_DATAPAGE_PER_WRITEENTRY 32

//struct dedup_node {
//	long long dedup_table_entry;
//};

// nova_dedup_queue.
// queue of entries that needs to be deduplicated.
struct nova_dedup_queue {
	u64 write_entry_address;
	u64 target_inode_number;
	struct list_head list;
};

// DEDUP - SHA1 Hashing Data Structures //
struct sdesc {
    struct shash_desc shash;
    char ctx[];
};

// FACT entry //
struct fact_entry {
	unsigned char fingerprint[FINGERPRINT_SIZE];
	u64 block_address;
	u32 count;		// 28bit - referenceCount, 4bit - updateCount.
	u32 next;
	u32 delete_target;
};

// For Fingerprint lookup. //
struct fingerprint_lookup_data {
	unsigned char fingerprint[FINGERPRINT_SIZE];	// fingerprint of entry
	u32 index;					// index of entry
	u64 block_address;				// Actual address of this entry (Where the data block is)
} __attribute((__packed__));

extern struct nova_dedup_queue nova_dedup_queue_head;

// NOVA_DEDUP_RADIX_TREE_NODE.
// leaf node of radix tree containing the address of matching dedup table entry.
//struct nova_dedup_radix_tree_node {
//	loff_t dedup_table_entry;
//};

// nova_dedup_table_entry.
// used to read from the dedup_table.
//struct dedup_table_entry {
//	char fingerprint[16];	// 16B
//	loff_t block_address;	// 8B
//	int reference_count;	// 4B
//	int flag;		// 4B
//};

// Dedup-queue related.
int nova_dedup_queue_init(void);
int nova_dedup_queue_push(u64, u64);	// parameter added.
u64 nova_dedup_queue_get_next_entry(u64 *);

// Radix-tree for searching D-table entry related.
//void nova_dedup_init_radix_tree_node (struct nova_dedup_radix_tree_node *, loff_t);

int nova_dedup_fingerprint (unsigned char *datapage, unsigned char *ret_fingerprint);
int nova_dedup_num_new_write_entry(short *target, int num_pages);
int nova_dedup_crosscheck(struct nova_file_write_entry *entry, struct nova_inode_info_header *sih, unsigned long pgoff);

int nova_dedup_FACT_update_count(struct super_block *sb, u64 index);
int nova_dedup_FACT_read(struct super_block *sb, u64 index);
int nova_dedup_is_empty(struct fact_entry target);
int nova_dedup_FACT_insert(struct super_block *sb, struct fingerprint_lookup_data *lookup);

int nova_dedup_entry_update(struct super_block *sb, struct nova_inode_info_header * sih, u64 begin_tail);
int nova_dedup_is_duplicate(struct super_block *sb, unsigned long blocknr, bool check);

// Debugging function for testing.
int nova_dedup_test (struct file*);

#endif
