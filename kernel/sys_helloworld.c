#include <linux/kernel.h>
#include <linux/syscalls.h>

SYSCALL_DEFINE0(helloworld)
//asmlinkage long sys_helloworld(void)
{
	printk("kernel HELLOWORLD\n");
	return 0;
}
