/* strdup lives in its own object so string.o never pulls in malloc. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char *strdup(const char *s)
{
	size_t n;
	char *p;

	if (!s)
		return 0;
	n = strlen(s) + 1;
	p = (char *)malloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}
