#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>

int real_dedup(struct file *file)
{	printk("deduping .2.\n");
	if (file->f_op->dedup) {
		printk("33333333\n");
		return file->f_op->dedup(file);
	}
	return 0;
}

int ksys_dedup (void)
//int ksys_dedup(unsigned int fd)
{	printk("deduping .1.\n");
	int ret = 0;
	struct file *filp = NULL;
//	struct fd f = fdget_pos(fd);	//	printk("1\n");
//	struct file *file = f.file;	//	printk("2\n");

	filp = filp_open("/mnt/nova/deduptatble", O_APPEND | O_RDWR | O_CREAT, 0);
	if (filp) {
		ret =  real_dedup(filp);
	}

//	return file->f_op->dedup(file);
	filp_close (filp, NULL);
	return ret;
}

SYSCALL_DEFINE0(dedup)
{
	printk("deduping .0.\n");
	return ksys_dedup();
}
