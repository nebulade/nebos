/* Host test for the stdio formatting core. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

int errno;

static int failures;

static void check(const char *got, const char *want)
{
	if (strcmp(got, want)) {
		printf("format mismatch: got [%s] want [%s]\n", got, want);
		failures++;
	}
}

int main(void)
{
	char b[2048];

	snprintf(b, sizeof(b), "%d %u %x %s %c %%", -42, 42u,
		 0xdeadbeefu, "hi", 'Z');
	check(b, "-42 42 deadbeef hi Z %");

	snprintf(b, sizeof(b), "%08d %-5s %.3s %+d % d",
		 42, "ab", "abcdef", 7, 7);
	check(b, "00000042 ab    abc +7  7");

	snprintf(b, sizeof(b), "%lld %llu %llx", 1234567890123LL,
		 18446744073709551615ULL, 0xdeadbeefcafebabeULL);
	check(b, "1234567890123 18446744073709551615 deadbeefcafebabe");

	snprintf(b, sizeof(b), "%f %.2f %.0f %e %.3e %g %.3g",
		 1.5, 1.005, 2.5, 1234.5, 0.0012345, 1000000.0, 0.00012345);
	check(b, "1.500000 1.00 2 1.234500e+03 1.234e-03 1e+06 0.000123");

	snprintf(b, sizeof(b), "%.20f", 1.0 / 3.0);
	check(b, "0.33333333333333331483");
	snprintf(b, sizeof(b), "%.20f", 0.1);
	check(b, "0.10000000000000000555");

	snprintf(b, sizeof(b), "%.0f %.0f %.0f %.0f", 0.5, 1.5, 2.5, 3.5);
	check(b, "0 2 2 4");
	snprintf(b, sizeof(b), "%.2f %.2f %.2f", 1.005, 2.675, 9.999);
	check(b, "1.00 2.67 10.00");

	snprintf(b, sizeof(b), "%g %g %g %g", 100000.0, 1000000.0,
		 0.0001, 0.00001);
	check(b, "100000 1e+06 0.0001 1e-05");

	snprintf(b, sizeof(b), "%f", -0.0);
	check(b, "-0.000000");
	{
		char *nullp = 0;
		const char *six = "abcdef";

		snprintf(b, sizeof(b), "%s", nullp);
		check(b, "(null)");
		snprintf(b, 5, "%s", six);
		check(b, "abcd");
	}

	snprintf(b, sizeof(b), "%p", (void *)0x1234);
	check(b, "0x1234");

	if (failures)
		printf("format test: %d failure(s)\n", failures);
	else
		printf("format test: OK\n");
	return failures != 0;
}
