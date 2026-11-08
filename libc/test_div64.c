/* Host test for the compiler 64-bit division helpers. */
#include <stdio.h>

typedef unsigned long long u64;
typedef long long s64;

u64 __udivdi3(u64, u64);
u64 __umoddi3(u64, u64);
s64 __divdi3(s64, s64);
s64 __moddi3(s64, s64);
s64 __divmoddi4(s64, s64, s64 *);

static int failures;

static void ck(const char *name, u64 got, u64 want)
{
	if (got != want) {
		printf("div64 mismatch %s: got %llu want %llu\n",
		       name, got, want);
		failures++;
	}
}

int main(void)
{
	s64 r;

	ck("udiv", __udivdi3(1000000000000ULL, 7ULL),
	   1000000000000ULL / 7ULL);
	ck("umod", __umoddi3(1000000000000ULL, 7ULL),
	   1000000000000ULL % 7ULL);
	ck("div", (u64)__divdi3(-1000000000000LL, 7LL),
	   (u64)(-1000000000000LL / 7LL));
	ck("mod", (u64)__moddi3(-1000000000000LL, 7LL),
	   (u64)(-1000000000000LL % 7LL));
	ck("divmodq", (u64)__divmoddi4(-1000000000000LL, 7LL, &r),
	   (u64)(-1000000000000LL / 7LL));
	ck("divmodr", (u64)r, (u64)(-1000000000000LL % 7LL));

	if (failures)
		printf("div64 test: %d failure(s)\n", failures);
	else
		printf("div64 test: OK\n");
	return failures != 0;
}
