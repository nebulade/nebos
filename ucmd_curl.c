/* curl: ring-3 command.  Fetch one http:// or https:// URL with the shared
 * uhttp client and dump the status line, headers and body to the terminal.
 *
 * The TLS and HTTP code lives in uhttp.c (shared with the browser); this file
 * only keeps the CLI and the verbose output hooks. */

#include "ucmd_lib.h"
#include "uhttp.h"

#define LINE_MAX_C 320

static void on_connected(void *ctx, int secure)
{
	(void)ctx;
	if (secure) {
		u_puts("curl: connected, TLS handshake\n");
	} else {
		u_puts("curl: connected, sending request\n");
	}
}

static void on_status(void *ctx, int code, const char *line)
{
	(void)ctx;
	(void)code;
	u_puts("curl: ");
	u_puts(line);
	u_putc('\n');
}

static void on_header(void *ctx, const char *line)
{
	(void)ctx;
	u_puts(line);
	u_putc('\n');
}

static void on_redirect(void *ctx, const char *location)
{
	(void)ctx;
	u_puts("curl: redirect -> ");
	u_puts(location);
	u_putc('\n');
}

static const struct uhttp_hooks hooks = {
	.connected = on_connected,
	.status = on_status,
	.header = on_header,
	.redirect = on_redirect,
};

void _start(const char *arg)
{
	static struct uhttp h;
	unsigned char buf[512];
	unsigned int total = 0;
	int rv;

	if (!arg || !arg[0]) {
		u_puts("curl: usage curl <http|https://host[:port]/[path]>\n");
		u_exit(1);
	}

	uhttp_init(&h);
	h.user_agent = "nebos-curl/0.1";

	u_puts("curl: GET ");
	u_puts(arg);
	u_putc('\n');

	rv = uhttp_open(&h, arg, 0, &hooks);
	if (rv != 0) {
		u_puts("curl: ");
		u_puts(uhttp_error_string(rv));
		u_putc('\n');
		uhttp_close(&h);
		u_exit(1);
	}
	if (h.url.secure)
		u_puts("curl: WARNING: certificate NOT verified "
		       "(trust-all mode)\n");

	u_putc('\n');
	for (;;) {
		rv = uhttp_read(&h, buf, sizeof(buf), UHTTP_IDLE_TIMEOUT_MS);
		if (rv > 0) {
			int i;

			for (i = 0; i < rv; i++)
				u_putc((char)buf[i]);
			total += (unsigned int)rv;
			continue;
		}
		if (rv < 0) {
			u_puts("\ncurl: ");
			u_puts(uhttp_error_string(rv));
			u_puts(" while reading body\n");
		}
		break;
	}
	u_putc('\n');
	u_puts("curl: ");
	u_putdec(total);
	u_puts(" bytes received, status ");
	u_putdec((unsigned int)h.resp.status);
	u_putc('\n');
	uhttp_close(&h);
	u_exit(0);
}
