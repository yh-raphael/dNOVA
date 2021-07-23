#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>

int ksys_dedup(unsigned int fd)
{
	printk("1\n");
	struct fd f = fdget_pos(fd);
	printk("2\n");
	struct file *file = f.file;
	printk("3\n");
	return file->f_op->dedup(file);
}

SYSCALL_DEFINE1(dedup, unsigned int, fd) {
	printk("deduping .0.\n");
	return ksys_dedup(fd);
}
