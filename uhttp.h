/* uhttp: small ring-3 HTTP/1.1 GET client shared by curl and the browser.
 *
 * The kernel owns the NIC and the TCP stack; this module drives them through
 * the SYS_NET_* / SYS_TCP_* calls, optionally wraps the connection in TLS
 * (tls.c), follows redirects and de-chunks the response.  The caller supplies
 * the connection object, so there is no allocation.
 *
 * https uses the phase-A tls.c client: encrypted but WITHOUT certificate
 * verification.  The caller can show a warning (curl does).
 */
#ifndef UHTTP_H
#define UHTTP_H

#include "ucmd_lib.h"

#define UHTTP_HOST_MAX       128
#define UHTTP_PATH_MAX       192
#define UHTTP_LOC_MAX        256
#define UHTTP_LINE_MAX       320
#define UHTTP_READER_MAX     2048
#define UHTTP_MAX_REDIRECTS  5
#define UHTTP_IDLE_TIMEOUT_MS  10000
#define UHTTP_TOTAL_TIMEOUT_MS 60000

/* uhttp_open flags. */
#define UHTTP_GZIP  1   /* send Accept-Encoding: gzip (caller decodes) */

/* Error codes (uhttp_open/uhttp_read return 0 on success). */
#define UHTTP_ERR_URL        (-1)
#define UHTTP_ERR_NIC        (-2)
#define UHTTP_ERR_IP         (-3)
#define UHTTP_ERR_BUSY       (-4)
#define UHTTP_ERR_DNS        (-5)
#define UHTTP_ERR_ARP        (-6)
#define UHTTP_ERR_CONNECT    (-7)
#define UHTTP_ERR_REFUSED    (-8)
#define UHTTP_ERR_TLS        (-9)
#define UHTTP_ERR_SEND       (-10)
#define UHTTP_ERR_TIMEOUT    (-11)
#define UHTTP_ERR_RESET      (-12)
#define UHTTP_ERR_REDIRECT   (-13)
#define UHTTP_ERR_TOOMANY    (-14)
#define UHTTP_ERR_TOOLONG    (-15)

struct uhttp_url {
	char host[UHTTP_HOST_MAX];
	char path[UHTTP_PATH_MAX];
	unsigned int port;
	int secure;
};

struct uhttp_response {
	int status;
	int chunked;
	int have_length;
	unsigned int content_length;
	int gzip;                       /* Content-Encoding: gzip */
	char content_type[64];
	char content_encoding[16];
	char final_url[UHTTP_HOST_MAX + UHTTP_PATH_MAX + 16];
};

/* Optional callbacks; curl uses them for its existing verbose output, the
 * browser passes NULL. */
struct uhttp_hooks {
	void (*connected)(void *ctx, int secure);
	void (*status)(void *ctx, int code, const char *line);
	void (*header)(void *ctx, const char *line);
	void (*redirect)(void *ctx, const char *location);
	void *ctx;
};

struct uhttp {
	struct uhttp_url url;
	struct uhttp_response resp;
	const struct uhttp_hooks *hooks;
	const char *user_agent;

	unsigned char buf[UHTTP_READER_MAX];
	unsigned int len, pos;
	unsigned int total_deadline;
	int eof;
	int error;                      /* UHTTP_ERR_* once a read failed */
	int connected;
	int owns_nic;

	/* chunked transfer state */
	int chunk_state;                /* 0 size line, 1 data, 2 crlf, 3 trailers */
	unsigned int chunk_left;
	unsigned int body_read;
};

void uhttp_init(struct uhttp *h);
int  uhttp_parse_url(const char *s, struct uhttp_url *u);
int  uhttp_open(struct uhttp *h, const char *url, unsigned int flags,
		const struct uhttp_hooks *hooks);
int  uhttp_read(struct uhttp *h, void *buf, unsigned int max,
		unsigned int timeout_ms);
void uhttp_close(struct uhttp *h);
const char *uhttp_error_string(int err);

/* Resolve an href against a base URL (absolute, absolute-path, relative and
 * scheme-relative forms).  0 on success. */
int uhttp_resolve(const char *base, const char *href, char *out,
		  unsigned int cap);

#endif
