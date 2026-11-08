/* websrv: a minimal HTTP/1.1 + WebDAV server.
 *
 * The TCP stack and the NIC stay in the kernel (see tcp.c/sys_net.c); this
 * command owns the receiver, parses one request per connection (always
 * "Connection: close") and maps WebDAV onto the VFS:
 *
 *   /              the mounted filesystems (collections)
 *   /ramfs/...     read-only (the command pack)
 *   /floppy1/...   FAT, writable with 8.3 names only
 *   /share/...     writable RAM store with long names
 *
 * Methods: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH,
 * MOVE, LOCK, UNLOCK.  No authentication: this is a trusted-LAN service.
 *
 * A file body is buffered whole (4 MB, or 1 MB when the heap is small)
 * because the VFS has no open/read/write-at interface.
 */

#include "ucmd_lib.h"

#define REQ_MAX       4096u
#define BODY_MAX      (4u * 1024u * 1024u)
#define BODY_FALLBACK (1024u * 1024u)
#define PATH_MAX      192u
#define XML_MAX       3072u
#define CHUNK_MAX     8224u
#define SOCK_MS       1000u
#define HDR_TIMEOUT_MS 10000u
#define REQ_TIMEOUT_MS 60000u
#define MAX_MOUNTS    24u

static char hdr[REQ_MAX];
static unsigned int hdr_len;
static char rbuf[REQ_MAX];
static unsigned char *body;
static unsigned int body_max;
static char resp[1024];
static unsigned int resp_len;
static char xml[XML_MAX];
static unsigned int xml_len;
static char cbuf[CHUNK_MAX];
static char batch[8192];
static unsigned int batch_len;
static struct sys_mount mounts[MAX_MOUNTS];
static unsigned int mount_count;
static unsigned int req_start;
static int status;
static const char *extra_hdrs;

struct req {
	char method[12];
	char target[PATH_MAX];
	char path[PATH_MAX];
	char dest[PATH_MAX];
	int  http11;
	int  have_len;
	unsigned int content_len;
	int  chunked;
	int  depth;
	int  expect_continue;
	int  overwrite;
	int  have_dest;
	int  keep_alive;
};

static struct req rq;

/* ---- small string helpers ---- */

static unsigned int s_len(const char *s)
{
	unsigned int n = 0;

	while (s[n])
		n++;
	return n;
}

static int s_eq(const char *a, const char *b)
{
	unsigned int i = 0;

	while (a[i] && b[i] && a[i] == b[i])
		i++;
	return a[i] == '\0' && b[i] == '\0';
}

static int s_ieq(const char *a, const char *b)
{
	unsigned int i = 0;

	for (;;) {
		char ca = a[i], cb = b[i];

		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
			return 0;
		if (!ca)
			return 1;
		i++;
	}
}

static int s_contains_ci(const char *hay, const char *needle)
{
	unsigned int i, j, n = s_len(needle);

	if (!n)
		return 1;
	for (i = 0; hay[i]; i++) {
		for (j = 0; j < n; j++) {
			char a = hay[i + j], b = needle[j];

			if (!a)
				return 0;
			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (j == n)
			return 1;
	}
	return 0;
}

static void s_copy(char *d, const char *s, unsigned int cap)
{
	unsigned int i = 0;

	while (i + 1 < cap && s[i]) {
		d[i] = s[i];
		i++;
	}
	d[i] = '\0';
}

static void mem_copy(void *d, const void *s, unsigned int n)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;

	while (n--)
		*dd++ = *ss++;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* ---- response buffer ---- */

static void ra(const char *s)
{
	while (*s && resp_len + 1 < sizeof(resp))
		resp[resp_len++] = *s++;
	resp[resp_len] = '\0';
}

static void rad(unsigned int v)
{
	char tmp[12];
	unsigned int i = sizeof(tmp) - 1;

	tmp[i] = '\0';
	if (!v) {
		ra("0");
		return;
	}
	while (v && i > 0) {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	}
	ra(tmp + i);
}

static const char *reason_phrase(int code)
{
	switch (code) {
	case 100: return "Continue";
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 207: return "Multi-Status";
	case 301: return "Moved Permanently";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 412: return "Precondition Failed";
	case 413: return "Payload Too Large";
	case 414: return "URI Too Long";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 507: return "Insufficient Storage";
	default:  return "Error";
	}
}

/* Send the status line + headers.  `extra` is appended verbatim (or NULL). */
static int send_headers(int code, const char *reason, const char *ctype,
			unsigned int len)
{
	resp_len = 0;
	ra("HTTP/1.1 ");
	rad((unsigned int)code);
	ra(" ");
	ra(reason);
	ra("\r\nServer: NebOS/0.2.0\r\n");
	ra((rq.keep_alive && code < 400) ? "Connection: keep-alive\r\n"
					 : "Connection: close\r\n");
	ra("Cache-Control: no-store\r\n");
	if (extra_hdrs) {
		ra(extra_hdrs);
		ra("\r\n");
	}
	if (ctype) {
		ra("Content-Type: ");
		ra(ctype);
		ra("\r\n");
	}
	ra("Content-Length: ");
	rad(len);
	ra("\r\n\r\n");
	status = code;
	return u_tcp_srv_send(resp, resp_len);
}

/* Small response with an optional body. */
static int send_body(int code, const char *reason, const char *ctype,
		     const void *data, unsigned int len)
{
	if (send_headers(code, reason, ctype, len) != 0)
		return -1;
	if (len && u_tcp_srv_send(data, len) != 0)
		return -1;
	return 0;
}

static int send_error(int code)
{
	char msg[64];
	unsigned int n = 0;
	const char *reason = reason_phrase(code);

	msg[n++] = (char)('0' + (code / 100) % 10);
	msg[n++] = (char)('0' + (code / 10) % 10);
	msg[n++] = (char)('0' + code % 10);
	msg[n++] = ' ';
	while (*reason && n + 2 < sizeof(msg))
		msg[n++] = *reason++;
	msg[n++] = '\n';
	return send_body(code, reason_phrase(code), "text/plain", msg, n);
}

static int send_empty(int code)
{
	return send_headers(code, reason_phrase(code), 0, 0);
}

/* Refuse a request and leave a human-readable reason in the terminal log
 * (the WebDAV client only sees the status code). */
static int send_refuse(int code, const char *why)
{
	u_puts("websrv: refused: ");
	u_puts(why);
	u_putc('\n');
	return send_error(code);
}

/* ---- chunked response (PROPFIND) ---- */

static int chunk_send(const char *p, unsigned int n)
{
	static const char hex[] = "0123456789abcdef";
	unsigned int l = 0, shift = 0, started = 0;

	if (n + 24 > sizeof(cbuf))
		return -1;
	if (!n) {
		mem_copy(cbuf, "0\r\n\r\n", 5);
		return u_tcp_srv_send(cbuf, 5);
	}
	/* hex size, most significant nibble first */
	for (shift = 28; ; shift -= 4) {
		unsigned int d = (n >> shift) & 0xF;

		if (d || started || shift == 0) {
			cbuf[l++] = hex[d];
			started = 1;
		}
		if (shift == 0)
			break;
	}
	cbuf[l++] = '\r';
	cbuf[l++] = '\n';
	mem_copy(cbuf + l, p, n);
	l += n;
	cbuf[l++] = '\r';
	cbuf[l++] = '\n';
	return u_tcp_srv_send(cbuf, l);
}

static const char multistatus_open[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	"<D:multistatus xmlns:D=\"DAV:\">";

static int flush_batch(void)
{
	if (!batch_len)
		return 0;
	if (chunk_send(batch, batch_len) != 0)
		return -1;
	batch_len = 0;
	return 0;
}

static int send_multistatus_begin(void)
{
	resp_len = 0;
	ra("HTTP/1.1 207 Multi-Status\r\n");
	ra("Server: NebOS/0.2.0\r\n");
	ra("Content-Type: text/xml; charset=\"utf-8\"\r\n");
	ra("Transfer-Encoding: chunked\r\n");
	ra(rq.keep_alive ? "Connection: keep-alive\r\n"
			 : "Connection: close\r\n");
	ra("Cache-Control: no-store\r\n\r\n");
	status = 207;
	if (u_tcp_srv_send(resp, resp_len) != 0)
		return -1;
	batch_len = 0;
	if (s_len(multistatus_open) > sizeof(batch))
		return -1;
	mem_copy(batch, multistatus_open, s_len(multistatus_open));
	batch_len = s_len(multistatus_open);
	return 0;
}

static int end_multistatus(void)
{
	static const char close_tag[] = "</D:multistatus>";
	unsigned int n = s_len(close_tag);

	if (batch_len + n > sizeof(batch) && flush_batch() != 0)
		return -1;
	mem_copy(batch + batch_len, close_tag, n);
	batch_len += n;
	if (flush_batch() != 0)
		return -1;
	return chunk_send("", 0);
}

/* ---- XML building ---- */

static void xa(const char *s)
{
	while (*s && xml_len + 1 < sizeof(xml))
		xml[xml_len++] = *s++;
	xml[xml_len] = '\0';
}

static void xa_esc(const char *s)
{
	while (*s && xml_len + 6 < sizeof(xml)) {
		char c = *s++;

		switch (c) {
		case '&':  xa("&amp;"); break;
		case '<':  xa("&lt;"); break;
		case '>':  xa("&gt;"); break;
		case '"':  xa("&quot;"); break;
		case '\'': xa("&apos;"); break;
		default:   xml[xml_len++] = c; break;
		}
	}
	xml[xml_len] = '\0';
}

static void xa_dec(unsigned int v)
{
	char tmp[12];
	unsigned int i = sizeof(tmp) - 1;

	tmp[i] = '\0';
	if (!v) {
		xa("0");
		return;
	}
	while (v && i > 0) {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	}
	xa(tmp + i);
}

/* Percent-encode a path for an href (keep '/' and unreserved characters). */
static void url_escape(const char *src, char *dst, unsigned int cap)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned int n = 0;

	while (*src && n + 4 < cap) {
		unsigned char c = (unsigned char)*src++;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '/' || c == '-' ||
		    c == '_' || c == '.' || c == '~') {
			dst[n++] = (char)c;
		} else {
			dst[n++] = '%';
			dst[n++] = hex[c >> 4];
			dst[n++] = hex[c & 0xF];
		}
	}
	dst[n] = '\0';
}

/* ---- request reader ---- */

static unsigned char *rptr;
static unsigned int rlen, rpos;

static void rb_init(unsigned int off)
{
	rptr = (unsigned char *)hdr + off;
	rlen = hdr_len - off;
	rpos = 0;
}

static int rb_read(unsigned char *dst, unsigned int n)
{
	while (n) {
		unsigned int avail = rlen - rpos;

		if (!avail) {
			int r = u_tcp_srv_recv(rbuf, sizeof(rbuf), SOCK_MS);

			if (r <= 0)
				return -1;
			rptr = (unsigned char *)rbuf;
			rlen = (unsigned int)r;
			rpos = 0;
			continue;
		}
		{
			unsigned int k = avail < n ? avail : n;

			mem_copy(dst, rptr + rpos, k);
			rpos += k;
			dst += k;
			n -= k;
		}
		if (u_ticks_ms() - req_start > REQ_TIMEOUT_MS)
			return -1;
	}
	return 0;
}

static int rb_line(char *out, unsigned int cap)
{
	unsigned int n = 0;

	for (;;) {
		unsigned char c;
		int r = rb_read(&c, 1);

		if (r != 0)
			return -1;
		if (c == '\n')
			break;
		if (n + 1 < cap)
			out[n++] = (char)c;
		else
			return -1;
	}
	if (n && out[n - 1] == '\r')
		n--;
	out[n] = '\0';
	return (int)n;
}

static int find_hdr_end(void)
{
	unsigned int i;

	if (hdr_len < 4)
		return -1;
	for (i = 0; i + 3 < hdr_len; i++)
		if (hdr[i] == '\r' && hdr[i + 1] == '\n' &&
		    hdr[i + 2] == '\r' && hdr[i + 3] == '\n')
			return (int)i;
	return -1;
}

/* A URL/file target -> absolute VFS path, no trailing slash, "" for root.
 * Accepts a path or an absolute URI (MOVE's Destination). */
static int decode_path(const char *src, char *out, unsigned int cap)
{
	const char *p = src;
	unsigned int o = 0;

	out[0] = '\0';
	if (p[0] != '/') {
		const char *q = p;

		while (*q && *q != ':' && *q != '/')
			q++;
		if (*q != ':')
			return -1;
		q++;
		while (*q == '/')
			q++;
		while (*q && *q != '/')
			q++;
		if (!*q) {
			out[0] = '\0';
			return 0;
		}
		p = q;
	}

	while (*p && *p != '?' && *p != '#') {
		char comp[PATH_MAX];
		unsigned int cl = 0;

		while (*p == '/')
			p++;
		while (*p && *p != '/' && *p != '?' && *p != '#') {
			int c = (unsigned char)*p++;

			if (c == '%') {
				int hi = p[0] ? hexval(p[0]) : -1;
				int lo = p[1] ? hexval(p[1]) : -1;

				if (hi < 0 || lo < 0)
					return -1;
				c = hi * 16 + lo;
				p += 2;
				if (c == 0 || c == '/')
					return -1;
			}
			if (cl + 1 >= sizeof(comp))
				return -1;
			comp[cl++] = (char)c;
		}
		comp[cl] = '\0';

		if (cl == 0 || (cl == 1 && comp[0] == '.'))
			continue;
		if (cl == 2 && comp[0] == '.' && comp[1] == '.') {
			while (o > 0 && out[o - 1] != '/')
				o--;
			if (o > 0)
				o--;
			out[o] = '\0';
			continue;
		}
		if (o + 1 + cl + 1 >= cap)
			return -1;
		out[o++] = '/';
		mem_copy(out + o, comp, cl);
		o += cl;
		out[o] = '\0';
	}
	return 0;
}

static void trim(char **s)
{
	char *p = *s;
	unsigned int n;

	while (*p == ' ' || *p == '\t')
		p++;
	n = s_len(p);
	while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
		p[--n] = '\0';
	*s = p;
}

/* Parse the request line and headers.  Returns 0 or an HTTP error code. */
static int parse_request(void)
{
	char *line = hdr, *next;
	char *sp1, *sp2;

	/* request line */
	next = line;
	while (*next && *next != '\n')
		next++;
	if (*next == '\n')
		*next++ = '\0';
	if (s_len(line) && line[s_len(line) - 1] == '\r')
		line[s_len(line) - 1] = '\0';

	sp1 = line;
	while (*sp1 && *sp1 != ' ')
		sp1++;
	if (!*sp1)
		return 400;
	*sp1++ = '\0';
	sp2 = sp1;
	while (*sp2 && *sp2 != ' ')
		sp2++;
	if (!*sp2)
		return 400;
	*sp2++ = '\0';

	if (s_len(line) >= sizeof(rq.method))
		return 414;
	s_copy(rq.method, line, sizeof(rq.method));
	if (s_len(sp1) >= sizeof(rq.target))
		return 414;
	s_copy(rq.target, sp1, sizeof(rq.target));
	rq.http11 = s_eq(sp2, "HTTP/1.1");
	rq.keep_alive = rq.http11 ? 1 : 0;

	if (s_eq(sp1, "*")) {
		rq.path[0] = '\0';
	} else if (decode_path(sp1, rq.path, sizeof(rq.path)) != 0) {
		return 400;
	}

	/* headers */
	rq.depth = 1;
	rq.overwrite = 1;
	for (line = next; *line; ) {
		char *end = line;
		char *colon, *value;

		while (*end && *end != '\n')
			end++;
		if (*end == '\n')
			*end++ = '\0';
		if (s_len(line) && line[s_len(line) - 1] == '\r')
			line[s_len(line) - 1] = '\0';
		if (!line[0])
			break;
		colon = line;
		while (*colon && *colon != ':')
			colon++;
		if (!*colon) {
			line = end;
			continue;
		}
		*colon++ = '\0';
		value = colon;
		trim(&value);

		if (s_ieq(line, "Content-Length")) {
			unsigned int v = 0;
			char *q = value;

			if (!*q)
				return 400;
			while (*q) {
				if (*q < '0' || *q > '9')
					return 400;
				if (v > 0xFFFFFFFFu / 10)
					return 413;
				v = v * 10 + (unsigned int)(*q - '0');
				q++;
			}
			rq.have_len = 1;
			rq.content_len = v;
		} else if (s_ieq(line, "Transfer-Encoding")) {
			if (s_contains_ci(value, "chunked"))
				rq.chunked = 1;
		} else if (s_ieq(line, "Depth")) {
			if (value[0] == '0')
				rq.depth = 0;
			else if (value[0] == '1')
				rq.depth = 1;
			else
				rq.depth = 1;   /* infinity: serve one level */
		} else if (s_ieq(line, "Destination")) {
			if (decode_path(value, rq.dest, sizeof(rq.dest)) == 0)
				rq.have_dest = 1;
		} else if (s_ieq(line, "Overwrite")) {
			rq.overwrite = !(value[0] == 'F' || value[0] == 'f');
		} else if (s_ieq(line, "Expect")) {
			if (s_contains_ci(value, "100-continue"))
				rq.expect_continue = 1;
		} else if (s_ieq(line, "Connection")) {
			if (s_contains_ci(value, "close"))
				rq.keep_alive = 0;
			else if (s_contains_ci(value, "keep-alive"))
				rq.keep_alive = 1;
		}
		line = end;
	}
	return 0;
}

static int read_request(void)
{
	unsigned int start = u_ticks_ms();

	req_start = start;
	hdr_len = 0;
	rq.method[0] = '\0';
	rq.target[0] = '\0';
	rq.path[0] = '\0';
	rq.dest[0] = '\0';
	rq.http11 = 0;
	rq.have_len = 0;
	rq.content_len = 0;
	rq.chunked = 0;
	rq.depth = 1;
	rq.expect_continue = 0;
	rq.overwrite = 1;
	rq.have_dest = 0;
	rq.keep_alive = 0;

	for (;;) {
		int end = find_hdr_end();

		if (end >= 0) {
			hdr[end] = '\0';
			{
				int r = parse_request();

				if (r != 0)
					return r;
			}
			/* One active TCP connection: gvfs/Nautilus open
			 * parallel ones, and an idle keep-alive connection
			 * would block their SYNs.  Close after each response. */
			rq.keep_alive = 0;
			rb_init((unsigned int)end + 4);
			return 0;
		}
		if (hdr_len >= REQ_MAX)
			return 431;
		if (u_ticks_ms() - start > HDR_TIMEOUT_MS)
			return 408;
		{
			int n = u_tcp_srv_recv(hdr + hdr_len,
					       REQ_MAX - hdr_len, SOCK_MS);

			if (n > 0) {
				hdr_len += (unsigned int)n;
			} else if (n == 0 || n == -2) {
				return hdr_len ? 400 : -2;
			} else if (hdr_len == 0 &&
				   u_ticks_ms() - start > 2000u) {
				/* Speculative connection with no request. */
				return -2;
			}
		}
	}
}

/* ---- body reader ---- */

/* 0 ok, -1 I/O/timeout, -2 larger than max.  Handles Content-Length and
 * chunked transfer encoding. */
static int read_content(unsigned char *dst, unsigned int max,
			unsigned int *out_len)
{
	unsigned int off = 0;

	*out_len = 0;
	if (rq.chunked) {
		for (;;) {
			char line[32];
			unsigned int sz = 0, i;
			int ln = rb_line(line, sizeof(line));

			if (ln <= 0)
				return -1;
			for (i = 0; line[i] && line[i] != ';'; i++) {
				int v = hexval(line[i]);

				if (v < 0)
					return -1;
				if (sz > (0xFFFFFFFFu >> 4))
					return -2;
				sz = (sz << 4) | (unsigned int)v;
			}
			if (sz == 0) {
				for (;;) {
					char t[32];
					int tl = rb_line(t, sizeof(t));

					if (tl < 0)
						return -1;
					if (tl == 0)
						break;
				}
				break;
			}
			if (off > max || sz > max - off)
				return -2;
			if (rb_read(dst + off, sz) != 0)
				return -1;
			off += sz;
			{
				unsigned char crlf[2];

				if (rb_read(crlf, 2) != 0)
					return -1;
				if (crlf[0] != '\r' || crlf[1] != '\n')
					return -1;
			}
		}
	} else if (rq.have_len) {
		if (rq.content_len > max)
			return -2;
		if (rq.content_len && rb_read(dst, rq.content_len) != 0)
			return -1;
		off = rq.content_len;
	}
	*out_len = off;
	return 0;
}

/* Read and discard a (small) request body so the response does not leave
 * unread data behind on the connection. */
static int drain_body(void)
{
	unsigned int n;
	int r = read_content(body, body_max < 65536u ? body_max : 65536u, &n);

	if (r == -2)
		return send_error(413), -1;
	if (r != 0)
		return send_error(400), -1;
	return 0;
}

/* Read and discard n bytes (a refused Content-Length body).  Responding 413
 * while the client is still writing would make it see a reset, which some
 * gvfs versions treat as a dead mount. */
static void discard_body(unsigned int n)
{
	unsigned char tmp[512];

	while (n) {
		unsigned int k = n > sizeof(tmp) ? sizeof(tmp) : n;

		if (rb_read(tmp, k) != 0)
			return;
		n -= k;
	}
}

/* ---- mounts and paths ---- */

static void load_mounts(void)
{
	mount_count = 0;
	while (mount_count < MAX_MOUNTS &&
	       u_syscall2(SYS_LIST_MOUNTS, (int)mount_count,
			  (int)&mounts[mount_count]) == 1)
		mount_count++;
}

static const char *vpath(const char *p)
{
	return p[0] ? p : "/";
}

/* Split "/mount/rel" into the mount name and the relative path.  Returns 0
 * for the root, 1 for a mount path, -1 for a malformed path. */
static int split_mount(const char *p, char *mount, unsigned int cap,
		       const char **rel)
{
	unsigned int i = 1, n = 0;

	if (p[0] != '/')
		return -1;
	if (!p[1])
		return 0;
	while (p[i] && p[i] != '/') {
		if (n + 1 >= cap)
			return -1;
		mount[n++] = p[i++];
	}
	mount[n] = '\0';
	*rel = p[i] ? p + i + 1 : p + i;
	return 1;
}

static struct sys_mount *mount_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < mount_count; i++)
		if (s_eq(mounts[i].name, name))
			return &mounts[i];
	return 0;
}

static struct sys_mount *mount_of(const char *p)
{
	char name[16];
	const char *rel;

	if (split_mount(p, name, sizeof(name), &rel) != 1)
		return 0;
	return mount_by_name(name);
}

static int path_ro(const char *p)
{
	struct sys_mount *m = mount_of(p);

	return !m || m->ro;
}

static int path_on_fat(const char *p)
{
	struct sys_mount *m = mount_of(p);

	return m && (s_ieq(m->type, "FAT12") || s_ieq(m->type, "FAT16") ||
		     s_ieq(m->type, "FAT32"));
}

/* "/share", "/ramfs", "/floppy1": the mount itself, not a file in it. */
static int is_mount_root(const char *p)
{
	char name[16];
	const char *rel;

	return split_mount(p, name, sizeof(name), &rel) == 1 && rel[0] == '\0';
}

static const char *base_name(const char *p)
{
	const char *b = p, *q = p;

	while (*q) {
		if (*q == '/')
			b = q + 1;
		q++;
	}
	return b;
}

static int join_path(const char *dir, const char *name, char *out,
		     unsigned int cap)
{
	unsigned int n = 0;

	if (dir[0]) {
		while (dir[n] && n + 1 < cap) {
			out[n] = dir[n];
			n++;
		}
	}
	if (n + 1 >= cap)
		return -1;
	out[n++] = '/';
	{
		unsigned int i = 0;

		while (name[i] && n + 1 < cap)
			out[n++] = name[i++];
	}
	out[n] = '\0';
	return 0;
}

static void parent_path(const char *p, char *out, unsigned int cap)
{
	int i = (int)s_len(p) - 1;

	while (i >= 0 && p[i] != '/')
		i--;
	if (i <= 0) {
		out[0] = '\0';
		return;
	}
	if ((unsigned int)i >= cap)
		i = (int)cap - 1;
	{
		int j;

		for (j = 0; j < i; j++)
			out[j] = p[j];
		out[i] = '\0';
	}
}

/* FAT accepts names that fat_name_to_8_3() can represent without silently
 * truncating: 1..8 base chars, optional 0..3 extension chars. */
static int fat_char_ok(char c)
{
	if (c >= 'A' && c <= 'Z')
		return 1;
	if (c >= 'a' && c <= 'z')
		return 1;
	if (c >= '0' && c <= '9')
		return 1;
	switch (c) {
	case '!': case '#': case '$': case '%': case '&': case '\'':
	case '(': case ')': case '-': case '@': case '^': case '_':
	case '`': case '{': case '}': case '~':
		return 1;
	}
	return 0;
}

static int fat_name_ok(const char *name)
{
	unsigned int i, base = 0, ext = 0;
	int dot = 0;

	for (i = 0; name[i]; i++) {
		if (name[i] == '.') {
			if (dot)
				return 0;
			dot = 1;
			continue;
		}
		if (!fat_char_ok(name[i]))
			return 0;
		if (dot)
			ext++;
		else
			base++;
	}
	if (base == 0 || base > 8 || ext > 3)
		return 0;
	return 1;
}

/* The last component must be representable when creating a FAT name. */
static int fat_path_ok(const char *p)
{
	const char *b = base_name(p);

	return *b && fat_name_ok(b);
}

static const char *content_type(const char *p)
{
	const char *d = p, *q = p;

	while (*q) {
		if (*q == '.')
			d = q;
		q++;
	}
	if (!*d)
		return "application/octet-stream";
	if (s_ieq(d, ".txt") || s_ieq(d, ".md") || s_ieq(d, ".c") ||
	    s_ieq(d, ".h") || s_ieq(d, ".log") || s_ieq(d, ".ini"))
		return "text/plain; charset=utf-8";
	if (s_ieq(d, ".html") || s_ieq(d, ".htm"))
		return "text/html; charset=utf-8";
	if (s_ieq(d, ".png"))
		return "image/png";
	if (s_ieq(d, ".jpg") || s_ieq(d, ".jpeg"))
		return "image/jpeg";
	if (s_ieq(d, ".gif"))
		return "image/gif";
	if (s_ieq(d, ".bmp"))
		return "image/bmp";
	if (s_ieq(d, ".tga"))
		return "image/x-tga";
	if (s_ieq(d, ".pnm") || s_ieq(d, ".ppm") || s_ieq(d, ".pgm"))
		return "image/x-portable-anymap";
	return "application/octet-stream";
}

static int http_from_vfs(int r)
{
	switch (r) {
	case SYS_EROFS:  return 403;
	case SYS_ENOENT: return 404;
	case SYS_ENODEV: return 404;
	case SYS_ENOFS:  return 404;
	case SYS_EINVAL: return 409;
	case SYS_ENOSPC: return 507;
	case SYS_EWRITE: return 507;
	default:         return 500;
	}
}

/* ---- WebDAV helpers ---- */

static int emit_prop(const char *p, int is_dir, unsigned int size,
		     const char *display)
{
	char href[PATH_MAX * 3];

	url_escape(p[0] ? p : "/", href, sizeof(href));
	if (is_dir && href[s_len(href) - 1] != '/') {
		unsigned int n = s_len(href);

		if (n + 1 < sizeof(href)) {
			href[n++] = '/';
			href[n] = '\0';
		}
	}
	xml_len = 0;
	xa("<D:response><D:href>");
	xa_esc(href);
	xa("</D:href><D:propstat><D:prop><D:resourcetype>");
	if (is_dir)
		xa("<D:collection/>");
	xa("</D:resourcetype>");
	if (!is_dir) {
		xa("<D:getcontentlength>");
		xa_dec(size);
		xa("</D:getcontentlength><D:getcontenttype>");
		xa_esc(content_type(p));
		xa("</D:getcontenttype>");
	}
	xa("<D:displayname>");
	xa_esc(display);
	xa("</D:displayname>");
	xa("<D:getlastmodified>Thu, 01 Jan 2026 00:00:00 GMT</D:getlastmodified>");
	xa("</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>");
	xa("</D:response>");
	if (xml_len > sizeof(batch))
		return -1;
	if (batch_len + xml_len > sizeof(batch) && flush_batch() != 0)
		return -1;
	mem_copy(batch + batch_len, xml, xml_len);
	batch_len += xml_len;
	return 0;
}

static int list_child(unsigned int index, const char *dir, char *out,
		      int *is_dir, unsigned int *size)
{
	struct sys_dirent d;
	char disp[64];

	if (u_syscall3(SYS_LIST_DIR, (int)vpath(dir), (int)index,
		       (int)&d) != 1)
		return 0;
	if (d.raw_83)
		u_fat_name(d.name, disp, sizeof(disp));
	else
		s_copy(disp, d.name, sizeof(disp));
	if (!disp[0])
		return 0;
	if (join_path(dir, disp, out, PATH_MAX) != 0)
		return 0;
	*is_dir = (int)d.is_dir;
	*size = d.size;
	return 1;
}

/* ---- method handlers ---- */

static void do_options2(void)
{
	resp_len = 0;
	ra("HTTP/1.1 200 OK\r\nServer: NebOS/0.2.0\r\n");
	ra("DAV: 1, 2\r\nMS-Author-Via: DAV\r\n");
	ra("Allow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, "
	   "PROPPATCH, MOVE, LOCK, UNLOCK\r\n");
	ra(rq.keep_alive ? "Connection: keep-alive\r\n"
			 : "Connection: close\r\n");
	ra("Cache-Control: no-store\r\n");
	ra("Content-Length: 0\r\n\r\n");
	status = 200;
	u_tcp_srv_send(resp, resp_len);
}

static void do_get(int head)
{
	struct sys_stat st;
	unsigned int off;

	if (!rq.path[0]) {
		send_error(405);
		return;
	}
	if (u_stat(vpath(rq.path), &st) != 0) {
		send_error(404);
		return;
	}
	if (st.is_dir) {
		send_error(405);
		return;
	}
	if (st.size > body_max) {
		send_error(413);
		return;
	}
	{
		int n = u_syscall3(SYS_READ_FILE, (int)rq.path, (int)body,
				   (int)body_max);

		if (n < 0) {
			send_error(http_from_vfs(n));
			return;
		}
		if (send_headers(200, "OK", content_type(rq.path),
				 (unsigned int)n) != 0)
			return;
		if (head)
			return;
		for (off = 0; off < (unsigned int)n; ) {
			unsigned int k = (unsigned int)n - off;

			if (k > 65536u)
				k = 65536u;
			if (u_tcp_srv_send(body + off, k) != 0)
				return;
			off += k;
		}
	}
}

static void do_put(void)
{
	struct sys_stat st;
	char parent[PATH_MAX];
	unsigned int len;
	int existed = 0, r;

	if (!rq.path[0]) {
		send_error(403);
		return;
	}
	if (!rq.have_len && !rq.chunked) {
		send_error(411);
		return;
	}
	if (rq.have_len && rq.content_len > body_max) {
		/* Drain a body that is already on the wire (not when the client
		 * is still waiting for 100 Continue), then refuse cleanly. */
		if (!rq.expect_continue &&
		    rq.content_len <= 64u * 1024u * 1024u)
			discard_body(rq.content_len);
		send_error(413);
		return;
	}
	if (path_ro(rq.path)) {
		send_error(403);
		return;
	}
	if (path_on_fat(rq.path) && !fat_path_ok(rq.path)) {
		send_refuse(409, "FAT name is not 8.3 (use /share)");
		return;
	}

	parent_path(rq.path, parent, sizeof(parent));
	if (parent[0]) {
		if (u_stat(vpath(parent), &st) != 0 || !st.is_dir) {
			send_refuse(409, "PUT parent directory is missing");
			return;
		}
	}
	if (u_stat(vpath(rq.path), &st) == 0) {
		if (st.is_dir) {
			send_refuse(409, "PUT target is a directory");
			return;
		}
		existed = 1;
	}

	if (rq.expect_continue && rq.http11)
		u_tcp_srv_send("HTTP/1.1 100 Continue\r\n\r\n", 25);

	r = read_content(body, body_max, &len);
	if (r == -2) {
		send_error(413);
		return;
	}
	if (r != 0) {
		send_error(400);
		return;
	}
	r = u_syscall3(SYS_WRITE_FILE, (int)rq.path, (int)body, (int)len);
	if (r != 0) {
		u_puts("websrv: write failed code ");
		u_putdec((unsigned int)-r);
		u_putc('\n');
		send_error(http_from_vfs(r));
		return;
	}
	send_empty(existed ? 204 : 201);
}

static void do_mkcol(void)
{
	struct sys_stat st;
	char parent[PATH_MAX];
	int r;

	if (!rq.path[0] || path_ro(rq.path)) {
		send_error(403);
		return;
	}
	if (path_on_fat(rq.path) && !fat_path_ok(rq.path)) {
		send_refuse(409, "MKCOL FAT name is not 8.3 (use /share)");
		return;
	}
	if (u_stat(vpath(rq.path), &st) == 0) {
		send_error(405);
		return;
	}
	parent_path(rq.path, parent, sizeof(parent));
	if (parent[0]) {
		if (u_stat(vpath(parent), &st) != 0 || !st.is_dir) {
			send_refuse(409, "MKCOL parent directory is missing");
			return;
		}
	}
	r = u_syscall1(SYS_MKDIR, (int)rq.path);
	if (r != 0) {
		u_puts("websrv: mkdir failed code ");
		u_putdec((unsigned int)-r);
		u_putc('\n');
		send_error(http_from_vfs(r));
		return;
	}
	send_empty(201);
}

static int delete_tree(const char *p, unsigned int depth)
{
	struct sys_stat st;
	unsigned int i;
	int r;

	if (depth > 8)
		return SYS_EINVAL;
	if (u_stat(vpath(p), &st) != 0)
		return SYS_ENOENT;
	if (st.is_dir) {
		for (i = 0; ; i++) {
			char child[PATH_MAX];
			int is_dir;
			unsigned int size;

			if (!list_child(i, p, child, &is_dir, &size))
				break;
			r = delete_tree(child, depth + 1);
			if (r != 0)
				return r;
		}
		return u_syscall1(SYS_RMDIR, (int)p);
	}
	return u_syscall1(SYS_UNLINK, (int)p);
}

static void do_delete(void)
{
	int r;

	if (!rq.path[0] || is_mount_root(rq.path) || path_ro(rq.path)) {
		send_error(403);
		return;
	}
	r = delete_tree(rq.path, 0);
	if (r != 0) {
		send_error(http_from_vfs(r));
		return;
	}
	send_empty(204);
}

static void do_move(void)
{
	struct sys_stat st, dst;
	unsigned int len;
	int existed = 0, r;

	if (!rq.path[0] || path_ro(rq.path)) {
		send_error(403);
		return;
	}
	if (!rq.have_dest || !rq.dest[0]) {
		send_error(400);
		return;
	}
	if (s_eq(rq.path, rq.dest)) {
		send_empty(204);
		return;
	}
	if (path_ro(rq.dest)) {
		send_error(403);
		return;
	}
	if (path_on_fat(rq.dest) && !fat_path_ok(rq.dest)) {
		send_refuse(409, "MOVE destination FAT name is not 8.3");
		return;
	}
	if (u_stat(vpath(rq.path), &st) != 0) {
		send_error(404);
		return;
	}
	if (st.is_dir) {
		send_error(501);   /* directory renames are not supported */
		return;
	}
	if (st.size > body_max) {
		send_error(413);
		return;
	}
	{
		char parent[PATH_MAX];

		parent_path(rq.dest, parent, sizeof(parent));
		if (parent[0] &&
		    (u_stat(vpath(parent), &dst) != 0 || !dst.is_dir)) {
			send_refuse(409, "MOVE destination parent is missing");
			return;
		}
	}
	if (u_stat(vpath(rq.dest), &dst) == 0) {
		if (dst.is_dir) {
			send_refuse(409, "MOVE destination is a directory");
			return;
		}
		if (!rq.overwrite) {
			send_error(412);
			return;
		}
		existed = 1;
	}
	r = u_syscall3(SYS_READ_FILE, (int)rq.path, (int)body,
		       (int)body_max);
	if (r < 0) {
		send_error(http_from_vfs(r));
		return;
	}
	len = (unsigned int)r;
	r = u_syscall3(SYS_WRITE_FILE, (int)rq.dest, (int)body, (int)len);
	if (r != 0) {
		send_error(http_from_vfs(r));
		return;
	}
	r = u_syscall1(SYS_UNLINK, (int)rq.path);
	if (r != 0 && r != SYS_ENOENT) {
		send_error(http_from_vfs(r));
		return;
	}
	send_empty(existed ? 204 : 201);
}

static void do_propfind(void)
{
	struct sys_stat st;
	unsigned int i;

	if (drain_body() != 0)
		return;

	if (!rq.path[0]) {
		if (send_multistatus_begin() != 0)
			return;
		if (emit_prop("", 1, 0, "/") != 0) {
			end_multistatus();
			return;
		}
		if (rq.depth) {
			for (i = 0; i < mount_count; i++) {
				char mpath[32];

				if (join_path("", mounts[i].name, mpath,
					      sizeof(mpath)) != 0)
					continue;
				if (emit_prop(mpath, 1, 0, mounts[i].name) != 0)
					break;
			}
		}
		end_multistatus();
		return;
	}

	if (u_stat(vpath(rq.path), &st) != 0) {
		send_error(404);
		return;
	}
	if (send_multistatus_begin() != 0)
		return;
	if (emit_prop(rq.path, st.is_dir, st.size, base_name(rq.path)) != 0) {
		end_multistatus();
		return;
	}
	if (st.is_dir && rq.depth) {
		for (i = 0; ; i++) {
			char child[PATH_MAX];
			int is_dir;
			unsigned int size;

			if (!list_child(i, rq.path, child, &is_dir, &size))
				break;
			if (emit_prop(child, is_dir, size,
				      base_name(child)) != 0)
				break;
		}
	}
	end_multistatus();
}

static void do_proppatch(void)
{
	char href[PATH_MAX * 3];
	unsigned int n = 0;

	if (drain_body() != 0)
		return;
	url_escape(rq.path[0] ? rq.path : "/", href, sizeof(href));
	xml_len = 0;
	xa("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
	xa("<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
	xa_esc(href);
	xa("</D:href><D:propstat><D:prop/>"
	   "<D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
	   "</D:response></D:multistatus>");
	n = xml_len;
	send_body(207, "Multi-Status", "text/xml; charset=\"utf-8\"",
		  xml, n);
}

static void do_lock(void)
{
	static const char lockbody[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
		"<D:locktype><D:write/></D:locktype>"
		"<D:depth>infinity</D:depth>"
		"<D:timeout>Second-3600</D:timeout>"
		"<D:locktoken><D:href>opaquelocktoken:nebs</D:href>"
		"</D:locktoken></D:activelock></D:lockdiscovery></D:prop>";

	(void)drain_body();
	extra_hdrs = "Lock-Token: <opaquelocktoken:nebs>";
	send_body(200, "OK", "text/xml; charset=\"utf-8\"",
		  lockbody, sizeof(lockbody) - 1);
}

/* ---- request dispatch ---- */

/* Serve one request.  Returns 1 when the connection should stay open for the
 * next request (HTTP keep-alive), 0 to close. */
static int serve(void)
{
	int r = read_request();

	status = 0;
	extra_hdrs = 0;
	if (r == -2)
		return 0;
	if (r != 0) {
		send_error(r);
		u_puts("websrv: bad request (");
		u_putdec((unsigned int)r);
		u_puts(")\n");
		return 0;
	}

	load_mounts();

	/* Methods with no body of their own: consume one if a client sent it,
	 * otherwise it would be parsed as the next request. */
	if ((s_eq(rq.method, "GET") || s_eq(rq.method, "HEAD") ||
	     s_eq(rq.method, "OPTIONS") || s_eq(rq.method, "DELETE") ||
	     s_eq(rq.method, "MKCOL") || s_eq(rq.method, "MOVE")) &&
	    (rq.have_len || rq.chunked)) {
		if (drain_body() != 0)
			return 0;
	}

	if (s_eq(rq.method, "OPTIONS"))
		do_options2();
	else if (s_eq(rq.method, "GET"))
		do_get(0);
	else if (s_eq(rq.method, "HEAD"))
		do_get(1);
	else if (s_eq(rq.method, "PUT"))
		do_put();
	else if (s_eq(rq.method, "DELETE"))
		do_delete();
	else if (s_eq(rq.method, "MKCOL"))
		do_mkcol();
	else if (s_eq(rq.method, "PROPFIND"))
		do_propfind();
	else if (s_eq(rq.method, "PROPPATCH"))
		do_proppatch();
	else if (s_eq(rq.method, "MOVE"))
		do_move();
	else if (s_eq(rq.method, "LOCK"))
		do_lock();
	else if (s_eq(rq.method, "UNLOCK")) {
		(void)drain_body();
		send_empty(204);
	} else {
		(void)drain_body();
		send_error(405);
	}

	if (status) {
		u_puts("websrv: ");
		u_puts(rq.method);
		u_putc(' ');
		u_puts(rq.path[0] ? rq.path : "/");
		u_puts(" -> ");
		u_putdec((unsigned int)status);
		u_putc('\n');
	}

	if (!status || status >= 400)
		return 0;
	/* Unconsumed bytes (HTTP pipelining, or a body we did not read) would
	 * be lost on the next read: close rather than desynchronise. */
	if (rlen > rpos)
		return 0;
	return rq.keep_alive ? 1 : 0;
}

static unsigned int parse_port(const char *arg)
{
	unsigned int p = 0;

	if (!arg)
		return 0;
	while (*arg) {
		if (*arg < '0' || *arg > '9')
			return 0;
		p = p * 10 + (unsigned int)(*arg - '0');
		if (p > 65535)
			return 0;
		arg++;
	}
	return p;
}

void _start(const char *arg)
{
	unsigned int port = parse_port(arg);
	unsigned char ip[4];

	if (!port)
		port = 8080;

	if (u_syscall0(SYS_NET_ACQUIRE) != 0) {
		u_puts("websrv: network busy (another command owns the NIC)\n");
		u_exit(1);
	}
	u_syscall1(SYS_NET_GET_IP, (int)ip);

	body = u_mem_alloc(BODY_MAX);
	body_max = BODY_MAX;
	if (!body) {
		body = u_mem_alloc(BODY_FALLBACK);
		body_max = BODY_FALLBACK;
	}
	if (!body) {
		u_puts("websrv: no memory for the transfer buffer\n");
		u_syscall0(SYS_NET_RELEASE);
		u_exit(1);
	}

	load_mounts();
	if (u_tcp_listen(port) != 0) {
		u_puts("websrv: cannot listen\n");
		u_syscall0(SYS_NET_RELEASE);
		u_exit(1);
	}

	u_puts("websrv: transfer buffer ");
	u_putdec(body_max / 1024u);
	u_puts(" KB\n");

	u_puts("websrv: serving http://");
	u_put_ip(ip);
	u_putc(':');
	u_putdec(port);
	u_puts("/ (WebDAV: dav://");
	u_put_ip(ip);
	u_putc(':');
	u_putdec(port);
	u_puts("/)\n");

	for (;;) {
		int r = u_tcp_accept();

		if (r == 1) {
			unsigned char peer[4];
			unsigned int pport = 0;

			if (u_tcp_peer(peer, &pport) == 0) {
				u_puts("websrv: connection from ");
				u_put_ip(peer);
				u_putc(':');
				u_putdec(pport);
				u_putc('\n');
			}
			while (serve())
				;
			u_tcp_srv_close();
			continue;
		}
		if (r == -3) {
			u_puts("websrv: netkexec requested, stopping\n");
			break;
		}
		if (r == -2)
			break;
		u_sleep_ms(2);
	}

	u_tcp_srv_close();
	u_syscall0(SYS_NET_RELEASE);
	u_puts("websrv: stopped\n");
	u_exit(0);
}
