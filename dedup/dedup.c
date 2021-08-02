#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>

int real_dedup(struct file *file)
{	printk("deduping .2.\n");
	if (file->f_op->dedup)
		return file->f_op->dedup(file);
	return 0;
}

int ksys_dedup(unsigned int fd)
{	printk("deduping .1.\n");
	struct fd f = fdget_pos(fd);	//	printk("1\n");
//	struct file *file = f.file;	//	printk("2\n");
	if (f.file)
		return real_dedup(f.file);
//	return file->f_op->dedup(file);
	return 0;
}

SYSCALL_DEFINE1(dedup, unsigned int, fd) {
	printk("deduping .0.\n");
	return ksys_dedup(fd);
}
