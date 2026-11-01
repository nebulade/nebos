/* uhttp: see uhttp.h.  Extracted from the original ucmd_curl.c so curl and
 * the browser share one HTTP/TLS path. */

#include "uhttp.h"
#include "tls.h"

/* ---- tiny string helpers ---- */

static unsigned int s_len(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void s_copy(char *d, const char *s, unsigned int cap)
{
	unsigned int i = 0;

	if (!cap)
		return;
	while (s && s[i] && i + 1 < cap) {
		d[i] = s[i];
		i++;
	}
	d[i] = '\0';
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

/* ---- URL parsing / resolution ---- */

int uhttp_parse_url(const char *s, struct uhttp_url *u)
{
	const char *p = s, *h;
	unsigned int i;
	int secure = 0;

	if (!s || !u)
		return UHTTP_ERR_URL;
	if (starts_with(s, "https://")) {
		secure = 1;
		p += 8;
	} else if (starts_with(s, "http://")) {
		p += 7;
	} else {
		for (i = 0; s[i] && i < 8; i++) {
			if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/')
				return UHTTP_ERR_URL;
			if (s[i] == '/')
				break;
		}
	}
	u->secure = secure;

	h = p;
	while (*p && *p != '/' && *p != ':')
		p++;
	if (p == h || (unsigned int)(p - h) >= UHTTP_HOST_MAX)
		return UHTTP_ERR_URL;
	for (i = 0; i < (unsigned int)(p - h); i++)
		u->host[i] = h[i];
	u->host[i] = '\0';

	u->port = secure ? 443 : 80;
	if (*p == ':') {
		unsigned int port = 0;

		p++;
		if (*p < '0' || *p > '9')
			return UHTTP_ERR_URL;
		while (*p >= '0' && *p <= '9') {
			port = port * 10 + (unsigned int)(*p - '0');
			if (port > 65535)
				return UHTTP_ERR_URL;
			p++;
		}
		if (port == 0 || (*p != '/' && *p != '\0'))
			return UHTTP_ERR_URL;
		u->port = port;
	}

	i = 0;
	if (*p == '/') {
		while (p[i]) {
			if (p[i] == '#')
				break;
			if (i + 1 >= UHTTP_PATH_MAX)
				return UHTTP_ERR_URL;
			u->path[i] = p[i];
			i++;
		}
	}
	u->path[i] = '\0';
	if (i == 0) {
		u->path[0] = '/';
		u->path[1] = '\0';
	}
	return 0;
}

static void url_build(const struct uhttp_url *u, char *out, unsigned int cap)
{
	unsigned int n = 0;

#define APP(s) do { const char *ap = (s); \
	while (*ap && n + 1 < cap) out[n++] = *ap++; } while (0)
	APP(u->secure ? "https://" : "http://");
	APP(u->host);
	if (u->port != (u->secure ? 443u : 80u)) {
		char tmp[8];
		unsigned int v = u->port, k = 0, j;

		if (v == 0) {
			tmp[k++] = '0';
		} else {
			while (v && k < sizeof(tmp)) {
				tmp[k++] = (char)('0' + v % 10);
				v /= 10;
			}
		}
		if (n + 1 < cap)
			out[n++] = ':';
		for (j = 0; j < k && n + 1 < cap; j++)
			out[n++] = tmp[k - 1 - j];
	}
	APP(u->path);
#undef APP
	out[n] = '\0';
}

/* Normalize an absolute URL path in place: collapse "//", "." and "..".
 * 0 ok, -1 too long. */
static int path_normalize(char *path)
{
	char tmp[UHTTP_PATH_MAX];
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
		if (seglen == 2 && path[start] == '.' && path[start + 1] == '.') {
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

/* Apply a Location header to the current URL. */
static int url_apply_location(struct uhttp_url *u, const char *loc)
{
	char joined[UHTTP_PATH_MAX];
	char scheme[8];
	unsigned int i, dirlen = 1, n = 0;

	if (starts_with(loc, "http://") || starts_with(loc, "https://"))
		return uhttp_parse_url(loc, u);

	if (loc[0] == '/' && loc[1] == '/') {
		/* scheme-relative */
		s_copy(scheme, u->secure ? "https:" : "http:", sizeof(scheme));
		if (s_len(scheme) + s_len(loc) + 1 > UHTTP_LOC_MAX)
			return -1;
		{
			char full[UHTTP_LOC_MAX];
			unsigned int k = 0;
			while (scheme[k]) {
				full[k] = scheme[k];
				k++;
			}
			for (i = 0; loc[i] && k + 1 < sizeof(full); i++)
				full[k++] = loc[i];
			full[k] = '\0';
			return uhttp_parse_url(full, u);
		}
	}

	if (loc[0] == '/') {
		for (i = 0; loc[i]; i++) {
			if (i + 1 >= UHTTP_PATH_MAX)
				return -1;
			u->path[i] = loc[i];
		}
		u->path[i] = '\0';
		return path_normalize(u->path);
	}

	for (i = 0; u->path[i]; i++)
		if (u->path[i] == '/')
			dirlen = i + 1;
	while (loc[n])
		n++;
	if (dirlen + n + 1 > UHTTP_PATH_MAX)
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

int uhttp_resolve(const char *base, const char *href, char *out,
		  unsigned int cap)
{
	struct uhttp_url u;

	if (!base || !href || !out || cap == 0)
		return UHTTP_ERR_URL;
	if (uhttp_parse_url(base, &u) != 0)
		return UHTTP_ERR_URL;
	if (href[0] == '#') {
		s_copy(out, base, cap);
		return 0;
	}
	if (url_apply_location(&u, href) != 0)
		return UHTTP_ERR_URL;
	url_build(&u, out, cap);
	return 0;
}

/* ---- HTTP header helpers ---- */

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

/* ---- transport ---- */

static int xport_send(struct uhttp *h, const void *buf, unsigned int len)
{
	if (h->url.secure)
		return tls_send(buf, len);
	return (int)u_syscall2(SYS_TCP_SEND, (int)buf, (int)len);
}

static int xport_recv(struct uhttp *h, void *buf, unsigned int max,
		      unsigned int timeout_ms)
{
	if (h->url.secure)
		return tls_recv(buf, max, timeout_ms);
	return (int)u_syscall3(SYS_TCP_RECV, (int)buf, (int)max,
			       (int)timeout_ms);
}

static void transport_close(struct uhttp *h)
{
	if (!h->connected)
		return;
	if (h->url.secure)
		tls_close();
	u_syscall0(SYS_TCP_CLOSE);
	h->connected = 0;
}

/* Connect to h->url (TCP + TLS).  Keeps the NIC acquired. */
static int http_connect(struct uhttp *h)
{
	unsigned char ip[4], hop[4], mac[6];
	int rv;

	if (u_parse_ip(h->url.host, ip) != 0) {
		if (u_syscall2(SYS_DNS_RESOLVE, (int)h->url.host, (int)ip) != 0)
			return UHTTP_ERR_DNS;
	}
	u_syscall2(SYS_NET_NEXT_HOP, (int)ip, (int)hop);
	rv = u_syscall2(SYS_NET_ARP, (int)hop, (int)mac);
	if (rv != 0)
		return UHTTP_ERR_ARP;

	rv = u_syscall3(SYS_TCP_CONNECT, (int)mac, (int)ip, (int)h->url.port);
	if (rv == -1)
		return UHTTP_ERR_TIMEOUT;
	if (rv == -2)
		return UHTTP_ERR_REFUSED;
	if (rv != 0)
		return UHTTP_ERR_CONNECT;
	h->connected = 1;
	if (h->url.secure) {
		if (tls_connect(h->url.host) != 0) {
			transport_close(h);
			return UHTTP_ERR_TLS;
		}
	}
	return 0;
}

/* ---- response reader ---- */

static int reader_fill(struct uhttp *h, unsigned int timeout_ms)
{
	unsigned int now, deadline, rel;
	int n;

	if (h->eof)
		return 0;
	now = u_ticks_ms();
	deadline = now + timeout_ms;
	if ((int)(h->total_deadline - deadline) < 0)
		deadline = h->total_deadline;
	rel = (int)(deadline - now) > 0 ? deadline - now : 1;

	n = xport_recv(h, h->buf, sizeof(h->buf), rel);
	if (n > 0) {
		h->len = (unsigned int)n;
		h->pos = 0;
		return 1;
	}
	if (n == 0) {
		h->eof = 1;
		return 0;
	}
	h->error = (n == -1) ? UHTTP_ERR_TIMEOUT : UHTTP_ERR_RESET;
	return 0;
}

static int reader_getc(struct uhttp *h, unsigned int timeout_ms)
{
	if (h->pos >= h->len) {
		if (!reader_fill(h, timeout_ms))
			return -1;
	}
	return h->buf[h->pos++];
}

static int reader_readline(struct uhttp *h, char *out, unsigned int max,
			   unsigned int timeout_ms)
{
	unsigned int n = 0;
	int c;

	for (;;) {
		c = reader_getc(h, timeout_ms);
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

/* ---- request / response ---- */

static unsigned int req_app(char *buf, unsigned int pos, unsigned int cap,
			    const char *s)
{
	while (*s && pos + 1 < cap)
		buf[pos++] = *s++;
	buf[pos] = '\0';
	return pos;
}

static unsigned int req_app_dec(char *buf, unsigned int pos, unsigned int cap,
				unsigned int v)
{
	char b[12];
	int i = 0;

	if (v == 0) {
		if (pos + 1 < cap)
			buf[pos++] = '0';
		buf[pos] = '\0';
		return pos;
	}
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i) {
		i--;
		if (pos + 1 < cap)
			buf[pos++] = b[i];
	}
	buf[pos] = '\0';
	return pos;
}

/* Read the status line and headers of the current response.  Returns 0 and
 * sets redirect/location when the response is a redirect with a target. */
static int read_response_head(struct uhttp *h, char *location, int *have_loc)
{
	char line[UHTTP_LINE_MAX];
	unsigned int status = 0;
	int rv;

	h->resp.status = 0;
	h->resp.chunked = 0;
	h->resp.have_length = 0;
	h->resp.content_length = 0;
	h->resp.gzip = 0;
	h->resp.content_type[0] = '\0';
	h->resp.content_encoding[0] = '\0';
	h->peer_close = 0;
	h->chunk_state = 0;
	h->chunk_left = 0;
	h->body_read = 0;
	h->len = h->pos = 0;
	h->eof = 0;
	h->error = 0;
	*have_loc = 0;
	location[0] = '\0';

	rv = reader_readline(h, line, sizeof(line), UHTTP_IDLE_TIMEOUT_MS);
	if (rv < 0)
		return (h->error == UHTTP_ERR_RESET) ? UHTTP_ERR_RESET :
						       UHTTP_ERR_TIMEOUT;
	status = parse_status(line);
	h->resp.status = (int)status;
	if (h->hooks && h->hooks->status)
		h->hooks->status(h->hooks->ctx, (int)status, line);

	for (;;) {
		rv = reader_readline(h, line, sizeof(line), UHTTP_IDLE_TIMEOUT_MS);
		if (rv < 0)
			return (h->error == UHTTP_ERR_RESET) ? UHTTP_ERR_RESET :
							       UHTTP_ERR_TIMEOUT;
		if (rv == 0)
			break;
		if (h->hooks && h->hooks->header)
			h->hooks->header(h->hooks->ctx, line);
		if (hdr_match(line, "content-length"))
			h->resp.have_length =
				parse_dec_after_colon(line, &h->resp.content_length);
		else if (hdr_match(line, "transfer-encoding") &&
			 contains_ci(line, "chunked"))
			h->resp.chunked = 1;
		else if (hdr_match(line, "content-encoding")) {
			copy_after_colon(line, h->resp.content_encoding,
					 sizeof(h->resp.content_encoding));
			if (contains_ci(h->resp.content_encoding, "gzip"))
				h->resp.gzip = 1;
		}
		else if (hdr_match(line, "content-type")) {
			copy_after_colon(line, h->resp.content_type,
					 sizeof(h->resp.content_type));
		} else if (hdr_match(line, "location")) {
			copy_after_colon(line, location, UHTTP_LOC_MAX);
			*have_loc = location[0] != '\0';
		} else if (hdr_match(line, "connection") &&
			   contains_ci(line, "close")) {
			h->peer_close = 1;
		}
	}
	return 0;
}

static int send_request(struct uhttp *h, unsigned int flags)
{
	char req[640];
	unsigned int n = 0;
	int rv;

	n = req_app(req, 0, sizeof(req), "GET ");
	n = req_app(req, n, sizeof(req), h->url.path);
	n = req_app(req, n, sizeof(req), " HTTP/1.1\r\nHost: ");
	n = req_app(req, n, sizeof(req), h->url.host);
	if (h->url.port != (h->url.secure ? 443u : 80u)) {
		n = req_app(req, n, sizeof(req), ":");
		n = req_app_dec(req, n, sizeof(req), h->url.port);
	}
	n = req_app(req, n, sizeof(req), "\r\nUser-Agent: ");
	n = req_app(req, n, sizeof(req), h->user_agent);
	n = req_app(req, n, sizeof(req), "\r\nAccept: */*\r\n");
	if (flags & UHTTP_GZIP)
		n = req_app(req, n, sizeof(req), "Accept-Encoding: gzip\r\n");
	n = req_app(req, n, sizeof(req),
		      (h->keepalive && !h->peer_close) ?
		      "Connection: keep-alive\r\n\r\n" :
		      "Connection: close\r\n\r\n");

	if ((int)n > u_syscall0(SYS_TCP_PEER_MSS))
		return UHTTP_ERR_TOOLONG;
	rv = xport_send(h, req, n);
	if (rv != 0)
		return UHTTP_ERR_SEND;
	return 0;
}

void uhttp_init(struct uhttp *h)
{
	unsigned int i;

	for (i = 0; i < sizeof(*h); i++)
		((unsigned char *)h)[i] = 0;
	h->user_agent = "nebos-uhttp/0.1";
}

static int host_same(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int uhttp_open(struct uhttp *h, const char *url, unsigned int flags,
	       const struct uhttp_hooks *hooks)
{
	char location[UHTTP_LOC_MAX];
	char old_host[UHTTP_HOST_MAX];
	unsigned int old_port = 0;
	int old_secure = 0;
	unsigned int hops = 0;
	int reuse = 0;
	int rv;

	h->hooks = hooks;
	if (!h->user_agent)
		h->user_agent = "nebos-uhttp/0.1";
	if (h->keepalive && h->connected) {
		s_copy(old_host, h->url.host, sizeof(old_host));
		old_port = h->url.port;
		old_secure = h->url.secure;
		reuse = 1;
	}
	h->len = h->pos = 0;
	h->eof = 0;
	h->error = 0;
	h->peer_close = 0;

	if (uhttp_parse_url(url, &h->url) != 0)
		return UHTTP_ERR_URL;
	url_build(&h->url, h->resp.final_url, sizeof(h->resp.final_url));

	if (reuse && host_same(old_host, h->url.host) &&
	    old_port == h->url.port && old_secure == h->url.secure) {
		/* Keep the existing TCP/TLS connection. */
	} else {
		reuse = 0;
		if (h->connected)
			transport_close(h);
		if (!h->owns_nic) {
			struct sys_netinfo ni;

			if (u_syscall1(SYS_NET_INFO, (int)&ni) != 1)
				return UHTTP_ERR_NIC;
			if (u_syscall0(SYS_NET_CONFIGURED) != 1)
				return UHTTP_ERR_IP;
			if (u_syscall0(SYS_NET_ACQUIRE) != 0)
				return UHTTP_ERR_BUSY;
			h->owns_nic = 1;
		}
	}

	h->total_deadline = u_ticks_ms() + UHTTP_TOTAL_TIMEOUT_MS;

	for (;;) {
		int have_loc;
		int used_reuse = reuse;

		reuse = 0;
		for (;;) {
			if (!used_reuse) {
				rv = http_connect(h);
				if (rv != 0)
					goto fail;
				if (h->hooks && h->hooks->connected)
					h->hooks->connected(h->hooks->ctx,
							    h->url.secure);
			}
			rv = send_request(h, flags);
			if (rv == 0)
				rv = read_response_head(h, location,
							&have_loc);
			if (rv == 0)
				break;
			if (!used_reuse)
				goto fail;
			/* A kept-alive socket may have been closed by the
			 * server; reconnect and retry once. */
			used_reuse = 0;
			transport_close(h);
		}

		if (h->resp.status >= 300 && h->resp.status < 400 && have_loc) {
			if (h->hooks && h->hooks->redirect)
				h->hooks->redirect(h->hooks->ctx, location);
			if (hops >= UHTTP_MAX_REDIRECTS) {
				rv = UHTTP_ERR_TOOMANY;
				goto fail;
			}
			transport_close(h);
			if (url_apply_location(&h->url, location) != 0) {
				rv = UHTTP_ERR_REDIRECT;
				goto fail;
			}
			url_build(&h->url, h->resp.final_url,
				  sizeof(h->resp.final_url));
			hops++;
			continue;
		}
		break;
	}
	return 0;

fail:
	h->keepalive = 0;
	uhttp_close(h);
	return rv;
}

/* Copy up to max decoded body bytes; >0 data, 0 EOF, <0 error. */
int uhttp_read(struct uhttp *h, void *vbuf, unsigned int max,
	       unsigned int timeout_ms)
{
	unsigned char *out = (unsigned char *)vbuf;
	unsigned int n = 0;
	int c;

	if (h->error)
		return h->error;
	if (h->eof && h->chunk_state != 1 &&
	    !(h->resp.have_length && h->body_read < h->resp.content_length))
		return 0;
	if (max == 0)
		return 0;

	if (!h->resp.chunked && h->resp.have_length &&
	    h->body_read >= h->resp.content_length) {
		h->eof = 1;
		return 0;
	}

	while (n < max) {
		if (h->resp.chunked) {
			if (h->chunk_state == 0) {
				char line[64];
				unsigned int sz = 0, k = 0;
				int hl = reader_readline(h, line, sizeof(line),
							 timeout_ms);

				if (hl < 0) {
					if (h->eof)
						h->peer_close = 1;
					if (n)
						break;
					return h->error ? h->error : 0;
				}
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
					h->error = UHTTP_ERR_RESET;
					return n ? (int)n : h->error;
				}
				if (sz == 0) {
					h->chunk_state = 3;
				} else {
					h->chunk_left = sz;
					h->chunk_state = 1;
				}
			} else if (h->chunk_state == 1) {
				c = reader_getc(h, timeout_ms);
				if (c < 0) {
					if (h->eof)
						h->peer_close = 1;
					if (n)
						break;
					return h->error ? h->error : 0;
				}
				out[n++] = (unsigned char)c;
				h->body_read++;
				if (--h->chunk_left == 0)
					h->chunk_state = 2;
			} else if (h->chunk_state == 2) {
				(void)reader_getc(h, timeout_ms); /* CR */
				(void)reader_getc(h, timeout_ms); /* LF */
				h->chunk_state = 0;
			} else {
				char line[64];
				int hl = reader_readline(h, line, sizeof(line),
							 timeout_ms);

				if (hl <= 0) {
					h->eof = 1;
					h->peer_close = 1;
					break;
				}
			}
		} else {
			if (h->resp.have_length &&
			    h->body_read >= h->resp.content_length) {
				h->eof = 1;
				break;
			}
			c = reader_getc(h, timeout_ms);
			if (c < 0) {
				if (h->eof)
					h->peer_close = 1;
				if (n)
					break;
				return h->error ? h->error : 0;
			}
			out[n++] = (unsigned char)c;
			h->body_read++;
		}
	}
	return (int)n;
}

void uhttp_close(struct uhttp *h)
{
	if (h->keepalive && h->connected && !h->peer_close && !h->error)
		return;
	transport_close(h);
	if (h->owns_nic) {
		u_syscall0(SYS_NET_RELEASE);
		h->owns_nic = 0;
	}
}

const char *uhttp_error_string(int err)
{
	switch (err) {
	case 0:                  return "ok";
	case UHTTP_ERR_URL:      return "bad URL";
	case UHTTP_ERR_NIC:      return "no NIC found";
	case UHTTP_ERR_IP:       return "no IP address (run dhcp or netip)";
	case UHTTP_ERR_BUSY:     return "network busy";
	case UHTTP_ERR_DNS:      return "DNS lookup failed";
	case UHTTP_ERR_ARP:      return "no ARP reply";
	case UHTTP_ERR_CONNECT:  return "connection failed";
	case UHTTP_ERR_REFUSED:  return "connection refused";
	case UHTTP_ERR_TLS:      return "TLS handshake failed";
	case UHTTP_ERR_SEND:     return "request send failed";
	case UHTTP_ERR_TIMEOUT:  return "timeout";
	case UHTTP_ERR_RESET:    return "connection reset";
	case UHTTP_ERR_REDIRECT: return "bad redirect target";
	case UHTTP_ERR_TOOMANY:  return "too many redirects";
	case UHTTP_ERR_TOOLONG:  return "request too long";
	default:                 return "network error";
	}
}
