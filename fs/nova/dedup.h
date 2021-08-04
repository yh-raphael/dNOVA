#ifndef __DEDUP_H__
#define __DEDUP_H__

#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/falloc.h>
#include <asm/mman.h>
#include <linux/radix-tree.h>
#include <linux/list.h>

#include "nova.h"
#include "inode.h"

/*** FingerPrint ***/
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/crypto.h>


#define DATABLOCK_SIZE		4096
#define FINGERPRINT_SIZE	16

//struct dedup_node {
//	long long dedup_table_entry;
//};

// queue of entries that needs to be deduplicated.
struct nova_dedup_queue {
	u64 write_entry_address;
	struct list_head list;
};

// leaf node of radix tree containing the address of matching dedup table entry.
struct nova_dedup_radix_tree_node {
	loff_t dedup_table_entry;
};

// used to read from the dedup_table.
struct dedup_table_entry {
	char fingerprint[16];
	loff_t block_address;
	int reference_count;
	int flag;
};

// Debugging function for testing.
int nova_dedup_test (struct file*);

// Dedup-queue related.
int nova_dedup_queue_init (void);
int nova_dedup_queue_push (u64);
u64 nova_dedup_queue_get_next_entry (void);

// Radix-tree for searching D-table entry related.
void nova_dedup_init_radix_tree_node (struct nova_dedup_radix_tree_node *, loff_t);

void nova_fingerprint (char *, char *);

#endif
