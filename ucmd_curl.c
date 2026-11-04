/* curl: ring-3 command.  Fetch one http:// or https:// URL with the shared
 * uhttp client and dump the status line, headers and body to the terminal.
 *
 * It also has a headless `__fetch` mode used by the browser: read
 * /share/bfreq, fetch it, and write the body plus /share/bfmeta for the GUI
 * process.  That keeps DNS/TLS/body waits out of the browser's UI loop.
 *
 * The TLS and HTTP code lives in uhttp.c (shared with the browser); this file
 * only keeps the CLI, the helper mode and the verbose output hooks. */

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


/* ---- headless fetch helper for the browser ---------------------------- */

#define FETCH_REQ   "/share/bfreq"
#define FETCH_BODY  "/share/bfbody"
#define FETCH_META  "/share/bfmeta"
#define FETCH_DONE  "/share/bfdone"
#define FETCH_MAX   (2u * 1024u * 1024u)

static unsigned int c_len(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void app_ch(char *d, unsigned int *n, char c, unsigned int cap)
{
	if (*n + 1 < cap)
		d[(*n)++] = c;
	d[*n] = '\0';
}

static void app_str(char *d, unsigned int *n, const char *s, unsigned int cap)
{
	while (s && *s)
		app_ch(d, n, *s++, cap);
}

static void app_dec(char *d, unsigned int *n, unsigned int v, unsigned int cap)
{
	char tmp[12];
	unsigned int i = 0, j;

	do {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < sizeof(tmp));
	for (j = 0; j < i; j++)
		app_ch(d, n, tmp[i - 1 - j], cap);
}

static void fetch_fail(unsigned int status, int err, const char *ctype,
		       const char *enc, const char *final)
{
	char meta[768];
	unsigned int n = 0;

	app_dec(meta, &n, status, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_dec(meta, &n, (unsigned int)err, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_dec(meta, &n, 0, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_str(meta, &n, enc ? enc : "", sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_str(meta, &n, ctype ? ctype : "", sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_str(meta, &n, final ? final : "", sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_dec(meta, &n, 0, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	u_syscall3(SYS_WRITE_FILE, (int)FETCH_META, (int)meta, (int)n);
}

static void write_meta(unsigned int seq, int with_seq, struct uhttp *h,
		       int err, unsigned int body_len, unsigned int body_trunc)
{
	char meta[1024];
	unsigned int n = 0;

	if (with_seq) {
		app_dec(meta, &n, seq, sizeof(meta));
		app_ch(meta, &n, '\n', sizeof(meta));
	}
	app_dec(meta, &n, (unsigned int)h->resp.status, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_dec(meta, &n, (unsigned int)err, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_dec(meta, &n, body_trunc, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_str(meta, &n, h->resp.content_encoding, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_str(meta, &n, h->resp.content_type, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_str(meta, &n, h->resp.final_url, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	app_dec(meta, &n, body_len, sizeof(meta)); app_ch(meta, &n, '\n', sizeof(meta));
	u_syscall3(SYS_WRITE_FILE, (int)FETCH_META, (int)meta, (int)n);
}

/* Fetch one URL through a possibly reused keep-alive connection. */
static void helper_fetch(struct uhttp *h, const char *url, unsigned int cap,
			 unsigned char *buf, unsigned int *outlen,
			 unsigned int *outtrunc, int *outerr)
{
	unsigned int total = 0;
	int rv, trunc = 0, err = 0;

	*outlen = 0;
	*outtrunc = 0;
	*outerr = 0;
	rv = uhttp_open(h, url, UHTTP_GZIP, 0);
	if (rv == UHTTP_ERR_BUSY) {
		int tries;

		for (tries = 0; tries < 50 && rv == UHTTP_ERR_BUSY; tries++) {
			u_sleep_ms(20);
			rv = uhttp_open(h, url, UHTTP_GZIP, 0);
		}
	}
	if (rv != 0) {
		*outerr = rv;
		return;
	}
	for (;;) {
		rv = uhttp_read(h, buf + total, cap - total,
				UHTTP_IDLE_TIMEOUT_MS);
		if (rv > 0) {
			total += (unsigned int)rv;
			if (total >= cap) {
				trunc = 1;
				break;
			}
			continue;
		}
		if (rv < 0)
			err = rv;
		break;
	}
	*outlen = total;
	*outtrunc = (unsigned int)trunc;
	*outerr = err;
}

static void fetch_once(void)
{
	static struct uhttp h;
	char req[768];
	unsigned char *buf;
	unsigned int cap = 0;
	unsigned int body_len = 0, body_trunc = 0;
	unsigned int i;
	int rn, err = 0;

	rn = u_syscall3(SYS_READ_FILE, (int)FETCH_REQ, (int)req,
			(int)(sizeof(req) - 1));
	if (rn <= 0)
		u_exit(1);
	req[rn] = '\0';
	i = 0;
	while (req[i] >= '0' && req[i] <= '9') {
		cap = cap * 10 + (unsigned int)(req[i] - '0');
		i++;
	}
	while (req[i] == ' ')
		i++;
	while (rn > 0 && (req[rn - 1] == '\n' || req[rn - 1] == '\r'))
		req[--rn] = '\0';
	if (i >= (unsigned int)rn || !req[i]) {
		fetch_fail(0, -1, "", "", "");
		u_exit(1);
	}
	if (cap == 0 || cap > FETCH_MAX)
		cap = FETCH_MAX;

	buf = (unsigned char *)u_mem_alloc(FETCH_MAX);
	if (!buf) {
		fetch_fail(0, -1, "", "", "");
		u_exit(1);
	}

	uhttp_init(&h);
	h.user_agent = "Mozilla/5.0 (X11; Linux i686) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36";
	helper_fetch(&h, req + i, cap, buf, &body_len, &body_trunc, &err);
	if (body_len)
		u_syscall3(SYS_WRITE_FILE, (int)FETCH_BODY, (int)buf,
			   (int)body_len);
	write_meta(0, 0, &h, err, body_len, body_trunc);
	uhttp_close(&h);
	u_mem_free();
	u_exit(err ? 1 : 0);
}

/* Long-lived helper: the browser writes a numbered request, we fetch it
 * (reusing the same TCP/TLS connection whenever the host is unchanged) and
 * publish body + meta + a small done marker.  Idle instances exit and
 * release the NIC after ten seconds. */
static void fetch_serve(void)
{
	static struct uhttp h;
	char req[768];
	unsigned char *buf;
	unsigned int last_seq = 0, idle = 0;

	buf = (unsigned char *)u_mem_alloc(FETCH_MAX);
	if (!buf)
		u_exit(1);
	uhttp_init(&h);
	h.user_agent = "Mozilla/5.0 (X11; Linux i686) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36";
	h.keepalive = 1;

	for (;;) {
		unsigned int seq = 0, cap = 0, body_len = 0, body_trunc = 0;
		char done[16];
		unsigned int dn = 0;
		unsigned int i = 0;
		char *url;
		int rn, err = 0;

		rn = u_syscall3(SYS_READ_FILE, (int)FETCH_REQ, (int)req,
				(int)(sizeof(req) - 1));
		if (rn <= 0) {
			url = 0;
		} else {
			req[rn] = '\0';
			while (req[i] >= '0' && req[i] <= '9') {
				seq = seq * 10 + (unsigned int)(req[i] - '0');
				i++;
			}
			while (req[i] == ' ')
				i++;
			while (req[i] >= '0' && req[i] <= '9') {
				cap = cap * 10 + (unsigned int)(req[i] - '0');
				i++;
			}
			while (req[i] == ' ')
				i++;
			while (rn > 0 && (req[rn - 1] == '\n' ||
					  req[rn - 1] == '\r'))
				req[--rn] = '\0';
			url = (i < (unsigned int)rn) ? req + i : 0;
		}
		if (!url || !url[0] || seq == last_seq) {
			idle++;
			u_sleep_ms(50);
			if (idle >= 200)
				break;
			continue;
		}
		idle = 0;
		last_seq = seq;
		if (cap == 0 || cap > FETCH_MAX)
			cap = FETCH_MAX;

		helper_fetch(&h, url, cap, buf, &body_len, &body_trunc, &err);
		if (body_len)
			u_syscall3(SYS_WRITE_FILE, (int)FETCH_BODY, (int)buf,
				   (int)body_len);
		write_meta(seq, 1, &h, err, body_len, body_trunc);
		app_dec(done, &dn, seq, sizeof(done));
		u_syscall3(SYS_WRITE_FILE, (int)FETCH_DONE, (int)done, (int)dn);

		if (err || body_trunc || h.peer_close) {
			h.keepalive = 0;
			uhttp_close(&h);
			h.keepalive = 1;
		}
	}
	h.keepalive = 0;
	uhttp_close(&h);
	u_mem_free();
	u_exit(0);
}

void _start(const char *arg)
{
	static struct uhttp h;
	unsigned char buf[512];
	unsigned int total = 0;
	int rv;

	if (arg && c_len(arg) == 7 && arg[0] == '_' && arg[1] == '_') {
		if (arg[2] == 'f')
			fetch_once();
		else if (arg[2] == 's')
			fetch_serve();
	}
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
