/* curl: ring-3 command.  Fetch one http:// or https:// URL with a minimal
 * HTTP GET and dump the status line, the headers and the body to the terminal.
 *
 * https uses the tls.c client (TLS 1.3, ChaCha20-Poly1305, X25519) WITHOUT
 * certificate verification (phase A): the handshake is encrypted, but a
 * man-in-the-middle is not detected; the leaf fingerprint is printed for a
 * manual check.  Redirects (3xx + Location) are followed up to
 * MAX_REDIRECTS hops: absolute URLs, absolute paths and relative paths
 * (resolved against the current path, "."/".." collapsed).  No gzip; a
 * chunked response is decoded.  The TCP client (tcp.c) and the DNS resolver
 * stay in the kernel; the nic is owned for the run (SYS_NET_ACQUIRE) and
 * TCP goes through the SYS_TCP_* calls. */

#include "ucmd_lib.h"
#include "tls.h"

#define URL_HOST_MAX 128
#define URL_PATH_MAX 128
#define REQ_MAX      512
#define LINE_MAX_C   320
#define READER_MAX   1024
#define LOCATION_MAX 256
#define MAX_REDIRECTS 5

#define IDLE_TIMEOUT_MS  10000
#define TOTAL_TIMEOUT_MS 60000

struct url {
	char host[URL_HOST_MAX];
	char path[URL_PATH_MAX];
	unsigned int port;
	int secure;
};

static struct url u;

/* ---- transport: plain TCP or TLS ---- */

static int xport_send(const void *buf, unsigned int len)
{
	if (u.secure)
		return tls_send(buf, len);
	return (int)u_syscall2(SYS_TCP_SEND, (int)buf, (int)len);
}

static int xport_recv(void *buf, unsigned int max, unsigned int timeout_ms)
{
	if (u.secure)
		return tls_recv(buf, max, timeout_ms);
	return (int)u_syscall3(SYS_TCP_RECV, (int)buf, (int)max,
			       (int)timeout_ms);
}

static int starts_with(const char *s, const char *prefix)
{
	unsigned int i = 0;

	while (prefix[i]) {
		if (s[i] != prefix[i])
			return 0;
		i++;
	}
	return 1;
}

/* ---- URL parsing ---- */

/* Parse http:// or https:// host[:port][/path]; 0 on success, -1 error. */
static int url_parse(const char *s, struct url *u)
{
	const char *p = s, *h;
	unsigned int i;
	int secure = 0;

	if (starts_with(s, "https://")) {
		secure = 1;
		p += 8;
	} else if (starts_with(s, "http://")) {
		p += 7;
	} else {
		/* An unsupported scheme like ftp:// is easy to spot: a "://"
		 * within the first few characters. */
		for (i = 0; s[i] && i < 8; i++) {
			if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/')
				return -1;
			if (s[i] == '/')
				break;
		}
	}
	u->secure = secure;

	h = p;
	while (*p && *p != '/' && *p != ':')
		p++;
	if (p == h || (unsigned int)(p - h) >= URL_HOST_MAX)
		return -1;
	for (i = 0; i < (unsigned int)(p - h); i++)
		u->host[i] = h[i];
	u->host[i] = '\0';

	u->port = secure ? 443 : 80;
	if (*p == ':') {
		unsigned int port = 0;

		p++;
		if (*p < '0' || *p > '9')
			return -1;
		while (*p >= '0' && *p <= '9') {
			port = port * 10 + (unsigned int)(*p - '0');
			if (port > 65535)
				return -1;
			p++;
		}
		if (port == 0 || (*p != '/' && *p != '\0'))
			return -1;
		u->port = port;
	}

	if (*p == '/') {
		i = 0;
		while (p[i]) {
			if (i + 1 >= URL_PATH_MAX)
				return -1;
			u->path[i] = p[i];
			i++;
		}
		u->path[i] = '\0';
	} else {
		u->path[0] = '/';
		u->path[1] = '\0';
	}
	return 0;
}

/* ---- redirect target handling ---- */

/* Normalize an absolute URL path in place: collapse "//", "." and ".." and
 * keep the leading '/' plus a trailing one.  0 ok, -1 too long. */
static int path_normalize(char *path)
{
	char tmp[URL_PATH_MAX];
	unsigned int i, o = 1, len = 0, start, seglen, j;
	int trailing;

	while (path[len])
		len++;
	if (len == 0 || path[0] != '/')
		return 0;
	trailing = (len > 1 && path[len - 1] == '/');
	tmp[0] = '/';
	i = 1;
	while (path[i]) {
		while (path[i] == '/')
			i++;
		start = i;
		while (path[i] && path[i] != '/')
			i++;
		seglen = i - start;
		if (seglen == 0)
			break;
		if (seglen == 1 && path[start] == '.')
			continue;
		if (seglen == 2 && path[start] == '.' &&
		    path[start + 1] == '.') {
			while (o > 1 && tmp[o - 1] != '/')
				o--;
			if (o > 1)
				o--;
			continue;
		}
		if (o + seglen + 2 > sizeof(tmp))
			return -1;
		if (o > 1)
			tmp[o++] = '/';
		for (j = 0; j < seglen; j++)
			tmp[o++] = path[start + j];
	}
	if (trailing && o > 1 && tmp[o - 1] != '/') {
		if (o + 2 > sizeof(tmp))
			return -1;
		tmp[o++] = '/';
	}
	tmp[o] = '\0';
	for (i = 0; i <= o; i++)
		path[i] = tmp[i];
	return 0;
}

/* Apply a Location header to the current URL.  Supports absolute URLs,
 * absolute paths and relative paths (joined to the current path's
 * directory).  0 ok, -1 unparsable/too long. */
static int url_apply_location(struct url *u, const char *loc)
{
	char joined[URL_PATH_MAX];
	unsigned int i, dirlen = 1, n = 0;

	if (starts_with(loc, "http://") || starts_with(loc, "https://"))
		return url_parse(loc, u);

	if (loc[0] == '/') {
		if (loc[1] == '/')
			return -1;              /* scheme-relative: unsupported */
		for (i = 0; loc[i]; i++) {
			if (i + 1 >= URL_PATH_MAX)
				return -1;
			u->path[i] = loc[i];
		}
		u->path[i] = '\0';
		return path_normalize(u->path);
	}

	/* Relative: keep the directory part of the current path. */
	for (i = 0; u->path[i]; i++) {
		if (u->path[i] == '/')
			dirlen = i + 1;
	}
	while (loc[n])
		n++;
	if (dirlen + n + 1 > URL_PATH_MAX)
		return -1;
	for (i = 0; i < dirlen; i++)
		joined[i] = u->path[i];
	for (i = 0; i < n; i++)
		joined[dirlen + i] = loc[i];
	joined[dirlen + n] = '\0';
	for (i = 0; i <= dirlen + n; i++)
		u->path[i] = joined[i];
	return path_normalize(u->path);
}

/* ---- request building ---- */

static unsigned int req_app(char *buf, unsigned int pos, const char *s)
{
	while (*s && pos < REQ_MAX - 1)
		buf[pos++] = *s++;
	return pos;
}

static unsigned int req_app_dec(char *buf, unsigned int pos, unsigned int v)
{
	char b[12];
	int i = 0;

	if (v == 0) {
		if (pos < REQ_MAX - 1)
			buf[pos++] = '0';
		return pos;
	}
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i) {
		i--;
		if (pos < REQ_MAX - 1)
			buf[pos++] = b[i];
	}
	return pos;
}

/* ---- HTTP response reader (a refillable line/byte stream) ---- */

struct reader {
	unsigned char buf[READER_MAX];
	unsigned int len, pos;
	unsigned int total_deadline;
	int eof;
	int error;              /* 0, -1 timeout, -2 reset */
};

/* Returns 1 when new data (or EOF) is available, 0 on timeout/reset. */
static int reader_fill(struct reader *r)
{
	unsigned int now, idle, deadline, rel;
	int n;

	if (r->eof)
		return 0;

	now = u_ticks_ms();
	idle = now + IDLE_TIMEOUT_MS;
	deadline = idle < r->total_deadline ? idle : r->total_deadline;
	rel = (int)(deadline - now) > 0 ? deadline - now : 1;

	n = xport_recv(r->buf, sizeof(r->buf), rel);
	if (n > 0) {
		r->len = (unsigned int)n;
		r->pos = 0;
		return 1;
	}
	if (n == 0) {
		r->eof = 1;
		return 0;
	}
	r->error = n;
	return 0;
}

static int reader_getc(struct reader *r)
{
	if (r->pos >= r->len) {
		if (!reader_fill(r))
			return -1;
	}
	return r->buf[r->pos++];
}

/* Read up to (not including) the next '\n'; -1 on stream error/EOF. */
static int reader_readline(struct reader *r, char *out, unsigned int max)
{
	unsigned int n = 0;
	int c;

	for (;;) {
		c = reader_getc(r);
		if (c < 0) {
			out[n] = '\0';
			return (n == 0) ? -1 : (int)n;
		}
		if (c == '\n') {
			out[n] = '\0';
			return (int)n;
		}
		if (c == '\r')
			continue;
		if (n + 1 < max)
			out[n++] = (char)c;
	}
}

/* ---- HTTP pieces ---- */

/* Case-insensitive match of a header name; line must continue with ':'. */
static int hdr_match(const char *line, const char *name)
{
	unsigned int i;

	for (i = 0; name[i]; i++) {
		char a = line[i];

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != name[i])
			return 0;
	}
	return line[i] == ':';
}

static int parse_dec_after_colon(const char *line, unsigned int *val)
{
	unsigned int i = 0, v = 0;
	int digits = 0;

	while (line[i] && line[i] != ':')
		i++;
	if (line[i] != ':')
		return 0;
	i++;
	while (line[i] == ' ' || line[i] == '\t')
		i++;
	while (line[i] >= '0' && line[i] <= '9') {
		v = v * 10 + (unsigned int)(line[i] - '0');
		digits = 1;
		i++;
	}
	if (!digits)
		return 0;
	*val = v;
	return 1;
}

static void copy_after_colon(const char *line, char *out, unsigned int max)
{
	unsigned int i = 0, j = 0;

	while (line[i] && line[i] != ':')
		i++;
	if (line[i] == ':')
		i++;
	while (line[i] == ' ' || line[i] == '\t')
		i++;
	while (line[i] && j + 1 < max)
		out[j++] = line[i++];
	out[j] = '\0';
}

static int contains_ci(const char *hay, const char *needle)
{
	unsigned int i, j;

	for (i = 0; hay[i]; i++) {
		for (j = 0; needle[j]; j++) {
			char a = hay[i + j];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (a != needle[j])
				break;
		}
		if (!needle[j])
			return 1;
	}
	return 0;
}

static unsigned int parse_status(const char *line)
{
	unsigned int i = 0, v = 0, digits = 0;

	while (line[i] && line[i] != ' ')
		i++;
	if (line[i] == ' ')
		i++;
	while (line[i] >= '0' && line[i] <= '9') {
		v = v * 10 + (unsigned int)(line[i] - '0');
		digits++;
		i++;
	}
	return digits == 3 ? v : 0;
}

/* Dump the body, filtering '\r'.  With have_limit, stop after limit bytes. */
static unsigned int dump_body(struct reader *r, int have_limit,
			      unsigned int limit)
{
	unsigned int n = 0;
	int c;

	for (;;) {
		if (have_limit && n >= limit)
			break;
		c = reader_getc(r);
		if (c < 0)
			break;
		if (c == '\r')
			continue;
		u_putc((char)c);
		n++;
	}
	return n;
}

/* Decode a chunked body (hex size line, data, CRLF; size 0 ends it). */
static unsigned int dump_chunked(struct reader *r)
{
	unsigned int total = 0;
	char line[64];

	for (;;) {
		unsigned int sz = 0, k = 0, i;
		int hl = reader_readline(r, line, sizeof(line));

		if (hl <= 0)
			return total;
		while (line[k]) {
			char ch = line[k];
			unsigned int d;

			if (ch >= '0' && ch <= '9')
				d = (unsigned int)(ch - '0');
			else if (ch >= 'a' && ch <= 'f')
				d = (unsigned int)(ch - 'a' + 10);
			else if (ch >= 'A' && ch <= 'F')
				d = (unsigned int)(ch - 'A' + 10);
			else
				break;
			sz = sz * 16 + d;
			k++;
		}
		if (k == 0) {
			u_puts("curl: bad chunk size\n");
			return total;
		}
		if (sz == 0) {
			/* trailer lines up to the empty line */
			while (reader_readline(r, line, sizeof(line)) > 0)
				;
			return total;
		}
		for (i = 0; i < sz; i++) {
			int c = reader_getc(r);

			if (c < 0)
				return total;
			u_putc((char)c);
			total++;
		}
		reader_readline(r, line, sizeof(line));   /* the CRLF */
	}
}

void _start(const char *arg)
{
	static char req[REQ_MAX];
	static struct reader r;
	struct sys_netinfo ni;
	char line[LINE_MAX_C];
	char location[LOCATION_MAX];
	unsigned int status = 0, body = 0, content_len = 0;
	unsigned int hops = 0;
	int have_loc, have_len, chunked, rv, connected = 0, follow = 1;

	if (!arg || !arg[0]) {
		u_puts("curl: usage curl <http|https://host[:port]/[path]>\n");
		u_exit(1);
	}

	rv = url_parse(arg, &u);
	if (rv != 0) {
		u_puts("curl: bad URL\n");
		u_exit(1);
	}
	if (u_syscall1(SYS_NET_INFO, (int)&ni) != 1) {
		u_puts("curl: no NIC found\n");
		u_exit(1);
	}
	if (u_syscall0(SYS_NET_CONFIGURED) != 1) {
		u_puts("curl: no IP address (run dhcp or netip)\n");
		u_exit(1);
	}
	if (u_syscall0(SYS_NET_ACQUIRE) != 0) {
		u_puts("curl: NIC busy\n");
		u_exit(1);
	}

	for (;;) {
		unsigned char ip[4], hop[4], mac[6];
		unsigned int n;

		have_loc = have_len = chunked = 0;
		content_len = 0;

		u_puts(u.secure ? "curl: GET https://" : "curl: GET http://");
		u_puts(u.host);
		if (u.port != (u.secure ? 443u : 80u)) {
			u_puts(":");
			u_putdec(u.port);
		}
		u_puts(u.path);
		u_putc('\n');

		if (u_parse_ip(u.host, ip) != 0) {
			if (u_syscall2(SYS_DNS_RESOLVE, (int)u.host, (int)ip) != 0)
				goto out;
		}

		u_syscall2(SYS_NET_NEXT_HOP, (int)ip, (int)hop);
		rv = u_syscall2(SYS_NET_ARP, (int)hop, (int)mac);
		if (rv != 0) {
			u_puts("curl: no ARP reply from ");
			u_put_ip(hop);
			u_putc('\n');
			goto out;
		}

		rv = u_syscall3(SYS_TCP_CONNECT, (int)mac, (int)ip, (int)u.port);
		if (rv == -1) {
			u_puts("curl: connect timeout\n");
			goto out;
		}
		if (rv == -2) {
			u_puts("curl: connection refused\n");
			goto out;
		}
		if (rv != 0) {
			u_puts("curl: connect failed\n");
			goto out;
		}
		connected = 1;

		if (u.secure) {
			u_puts("curl: connected, TLS handshake\n");
			if (tls_connect(u.host) != 0)
				goto out;
			u_puts("curl: WARNING: certificate NOT verified "
			       "(trust-all mode)\n");
		} else {
			u_puts("curl: connected, sending request\n");
		}

		n = req_app(req, 0, "GET ");
		n = req_app(req, n, u.path);
		n = req_app(req, n, u.secure ? " HTTP/1.1\r\nHost: "
					     : " HTTP/1.0\r\nHost: ");
		n = req_app(req, n, u.host);
		if (u.port != (u.secure ? 443u : 80u)) {
			n = req_app(req, n, ":");
			n = req_app_dec(req, n, u.port);
		}
		n = req_app(req, n, "\r\nUser-Agent: nebos-curl/0.1\r\n"
				    "Accept: */*\r\nConnection: close\r\n\r\n");
		if ((int)n > u_syscall0(SYS_TCP_PEER_MSS)) {
			u_puts("curl: request too long\n");
			goto out;
		}
		rv = xport_send(req, n);
		if (rv != 0) {
			u_puts("curl: request send failed\n");
			goto out;
		}

		r.len = 0;
		r.pos = 0;
		r.eof = 0;
		r.error = 0;
		r.total_deadline = u_ticks_ms() + TOTAL_TIMEOUT_MS;

		rv = reader_readline(&r, line, sizeof(line));
		if (rv < 0) {
			u_puts("curl: no response (");
			u_puts(r.error == -2 ? "reset" : "timeout");
			u_puts(")\n");
			goto out;
		}
		status = parse_status(line);
		u_puts("curl: ");
		u_puts(line);
		u_putc('\n');

		for (;;) {
			rv = reader_readline(&r, line, sizeof(line));
			if (rv < 0) {
				u_puts("curl: connection lost in headers\n");
				goto out;
			}
			if (rv == 0)
				break;              /* end of headers */
			u_puts(line);
			u_putc('\n');
			if (hdr_match(line, "content-length"))
				have_len = parse_dec_after_colon(line,
								 &content_len);
			else if (hdr_match(line, "transfer-encoding") &&
				 contains_ci(line, "chunked"))
				chunked = 1;
			else if (hdr_match(line, "location")) {
				copy_after_colon(line, location,
						 sizeof(location));
				have_loc = 1;
			}
		}

		if (status < 300 || status >= 400 || !have_loc)
			break;                  /* final response: dump the body */

		if (hops >= MAX_REDIRECTS) {
			u_puts("curl: too many redirects\n");
			follow = 0;
			break;
		}
		u_puts("curl: redirect -> ");
		u_puts(location);
		u_putc('\n');
		tls_close();
		u_syscall0(SYS_TCP_CLOSE);
		connected = 0;
		if (url_apply_location(&u, location) != 0) {
			u_puts("curl: bad redirect target\n");
			follow = 0;
			break;
		}
		hops++;
	}

	if (follow && connected) {
		u_putc('\n');
		if (chunked)
			body = dump_chunked(&r);
		else
			body = dump_body(&r, have_len, content_len);
		u_putc('\n');

		if (r.error == -2)
			u_puts("curl: connection reset\n");
		else if (r.error == -1)
			u_puts("curl: timeout while reading body\n");

		u_puts("curl: ");
		u_putdec(body);
		u_puts(" bytes received, status ");
		u_putdec(status);
		u_putc('\n');
	}

out:
	if (connected) {
		tls_close();
		u_syscall0(SYS_TCP_CLOSE);
	}
	u_syscall0(SYS_NET_RELEASE);
	u_exit(0);
}
