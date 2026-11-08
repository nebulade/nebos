/* 32-bit host test for libc/setjmp.S. */
#include <stdio.h>
#include <setjmp.h>

static jmp_buf env;
static int count;

int main(void)
{
	int r = setjmp(env);

	count++;
	if (r == 0)
		longjmp(env, 7);
	if (r != 7 || count != 2) {
		printf("setjmp test: FAIL r=%d count=%d\n", r, count);
		return 1;
	}
	printf("setjmp test: OK\n");
	return 0;
}
