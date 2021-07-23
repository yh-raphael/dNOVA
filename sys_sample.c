#include <stdio.h>
#include <stdlib.h>
#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>

int main()
{
	FILE* fp = fopen("b.txt","rt");
//	char* buf = (char*)malloc(sizeof(char)*100);
	long int ret = syscall(335);
	printf("sys_dedup () returned %ld\n", ret);
	fclose(fp);
	return 0;
}
