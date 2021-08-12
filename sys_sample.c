#include <stdio.h>
#include <stdlib.h>
#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>

int main()
{
//	FILE* fpp = fopen("a.txt", "r");
//	FILE* fp = fopen("b.txt","rt");
//	char* buf = (char*)malloc(sizeof(char)*100);
//	printf("fileno: %d \n", fileno(fp));
//	unsigned int temp = fileno(fp);
//	int ret = syscall(335, temp);
	printf("executing user program!\n");
	int ret = syscall(335);
	printf("sys_dedup () returned %d \n", ret);
//	fclose(fp);
//	fclose(fpp);
	return 0;
}
