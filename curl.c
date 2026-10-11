/* curl: fetch one http:// URL with a minimal HTTP/1.0 GET and dump the
 * status line, the headers and the body to the terminal.
 *
 * Deliberately small: no TLS, no redirect following (a Location header is
 * reported), no gzip; a chunked response is decoded.  Uses the DNS resolver
 * (dns.c) and the TCP client (tcp.c); the NIC is owned for the whole run. */

#include "io.h"
#include "timer.h"
#include "netdev.h"
#include "net.h"
#include "dns.h"
#include "tcp.h"

#define URL_HOST_MAX 128
#define URL_PATH_MAX 128
#define REQ_MAX      512
#define LINE_MAX_C   320
#define READER_MAX   1024

#define IDLE_TIMEOUT_MS  10000
#define TOTAL_TIMEOUT_MS 60000

struct url {
	char host[URL_HOST_MAX];
	char path[URL_PATH_MAX];
	unsigned int port;
};

/* ---- small print helpers ---- */

static void put_dec(unsigned int v)
{
	char b[12];
	int i = 0;

	if (v == 0) {
		io_putchar('0');
		return;
	}
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i)
		io_putchar(b[--i]);
}

static void put_ip(const unsigned char *ip)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (i)
			io_putchar('.');
		put_dec(ip[i]);
	}
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

/* Parse http://host[:port][/path]; 0 on success, -1 error, -2 https. */
static int url_parse(const char *s, struct url *u)
{
	const char *p = s, *h;
	unsigned int i;

	if (starts_with(s, "https://"))
		return -2;
	if (starts_with(s, "http://"))
		p += 7;
	else {
		/* An unsupported scheme like ftp:// is easy to spot: a "://"
		 * within the first few characters. */
		for (i = 0; s[i] && i < 8; i++) {
			if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/')
				return -1;
			if (s[i] == '/')
				break;
		}
	}

	h = p;
	while (*p && *p != '/' && *p != ':')
		p++;
	if (p == h || (unsigned int)(p - h) >= URL_HOST_MAX)
		return -1;
	for (i = 0; i < (unsigned int)(p - h); i++)
		u->host[i] = h[i];
	u->host[i] = '\0';

	u->port = 80;
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
	struct tcp_conn *conn;
	unsigned char buf[READER_MAX];
	unsigned int len, pos;
	unsigned int total_deadline;
	int eof;
	int error;              /* 0, -1 timeout, -2 reset */
};

/* Returns 1 when new data (or EOF) is available, 0 on timeout/reset. */
static int reader_fill(struct reader *r)
{
	unsigned int deadline, idle;
	int n;

	if (r->eof)
		return 0;

	idle = get_ticks_ms() + IDLE_TIMEOUT_MS;
	deadline = idle < r->total_deadline ? idle : r->total_deadline;

	n = tcp_recv(r->conn, r->buf, sizeof(r->buf), deadline);
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
		io_putchar((char)c);
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
			io_putstr("curl: bad chunk size\n");
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
			io_putchar((char)c);
			total++;
		}
		reader_readline(r, line, sizeof(line));   /* the CRLF */
	}
}

/* ---- the command ---- */

void program_curl_main(const char *arg)
{
	netdev_acquire();
	{
		static struct url u;
		static char req[REQ_MAX];
		static struct reader r;
		const struct netdev *nd = netdev_active();
		unsigned char ip[4], hop[4], mac[6];
		struct tcp_conn conn;
		char line[LINE_MAX_C];
		char location[128];
		unsigned int n, status = 0, body = 0, content_len = 0;
		int have_loc = 0, have_len = 0, chunked = 0, rv;

		if (arg[0] == '\0') {
			io_putstr("curl: usage curl <http://host[:port]/[path]>\n");
			goto out;
		}
		rv = url_parse(arg, &u);
		if (rv == -2) {
			io_putstr("curl: https is not supported (no TLS)\n");
			goto out;
		}
		if (rv != 0) {
			io_putstr("curl: bad URL\n");
			goto out;
		}
		if (!nd) {
			io_putstr("curl: no NIC found\n");
			goto out;
		}
		if (!net_is_configured()) {
			io_putstr("curl: no IP address (run dhcp or netip)\n");
			goto out;
		}

		io_putstr("curl: GET http://");
		io_putstr(u.host);
		if (u.port != 80) {
			io_putstr(":");
			put_dec(u.port);
		}
		io_putstr(u.path);
		io_putchar('\n');

		if (net_parse_ip(u.host, ip) != 0) {
			if (dns_resolve(u.host, ip) != 0)
				goto out;
		}

		net_next_hop(ip, hop);
		if (net_arp_resolve(hop, mac) != 0) {
			io_putstr("curl: no ARP reply from ");
			put_ip(hop);
			io_putchar('\n');
			goto out;
		}

		rv = tcp_connect(mac, ip, u.port, &conn);
		if (rv == -1) {
			io_putstr("curl: connect timeout\n");
			goto out;
		}
		if (rv == -2) {
			io_putstr("curl: connection refused\n");
			goto out;
		}
		if (rv != 0) {
			io_putstr("curl: connect failed\n");
			goto out;
		}
		io_putstr("curl: connected, sending request\n");

		n = req_app(req, 0, "GET ");
		n = req_app(req, n, u.path);
		n = req_app(req, n, " HTTP/1.0\r\nHost: ");
		n = req_app(req, n, u.host);
		if (u.port != 80) {
			n = req_app(req, n, ":");
			n = req_app_dec(req, n, u.port);
		}
		n = req_app(req, n, "\r\nUser-Agent: nebos-curl/0.1\r\n"
				    "Accept: */*\r\nConnection: close\r\n\r\n");
		if (n > conn.peer_mss) {
			io_putstr("curl: request too long\n");
			goto out;
		}
		rv = tcp_send(&conn, req, n);
		if (rv != 0) {
			io_putstr("curl: request send failed\n");
			goto out;
		}

		r.conn = &conn;
		r.len = 0;
		r.pos = 0;
		r.eof = 0;
		r.error = 0;
		r.total_deadline = get_ticks_ms() + TOTAL_TIMEOUT_MS;

		rv = reader_readline(&r, line, sizeof(line));
		if (rv < 0) {
			io_putstr("curl: no response (");
			io_putstr(r.error == -2 ? "reset" : "timeout");
			io_putstr(")\n");
			goto out;
		}
		status = parse_status(line);
		io_putstr("curl: ");
		io_putstr(line);
		io_putchar('\n');

		for (;;) {
			rv = reader_readline(&r, line, sizeof(line));
			if (rv < 0) {
				io_putstr("curl: connection lost in headers\n");
				goto out;
			}
			if (rv == 0)
				break;              /* end of headers */
			io_putstr(line);
			io_putchar('\n');
			if (hdr_match(line, "content-length"))
				have_len = parse_dec_after_colon(line, &content_len);
			else if (hdr_match(line, "transfer-encoding") &&
				 contains_ci(line, "chunked"))
				chunked = 1;
			else if (hdr_match(line, "location")) {
				copy_after_colon(line, location,
						 sizeof(location));
				have_loc = 1;
			}
		}

		if (status >= 300 && status < 400 && have_loc) {
			io_putstr("curl: redirect not followed: ");
			io_putstr(location);
			io_putchar('\n');
		}

		io_putchar('\n');
		if (chunked)
			body = dump_chunked(&r);
		else
			body = dump_body(&r, have_len, content_len);
		io_putchar('\n');

		if (r.error == -2)
			io_putstr("curl: connection reset\n");
		else if (r.error == -1)
			io_putstr("curl: timeout while reading body\n");

		io_putstr("curl: ");
		put_dec(body);
		io_putstr(" bytes received, status ");
		put_dec(status);
		io_putstr("\n");

		tcp_close(&conn);
	}
out:
	netdev_release();
}
