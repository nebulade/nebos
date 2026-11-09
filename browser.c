/* browser: a small HTML/CSS browser built on ngui, the image library and the
 * shared uhttp client.
 *
 * Inline JavaScript runs through the vendored MuJS engine (console.log,
 * alert, document.getElementById(...).textContent and location.href); forms
 * are not supported.  External CSS is loaded over uhttp, and images are
 * decoded with img.c and drawn inline.  Page-scoped buffers (page body,
 * rules and image bitmaps) come from the ring-3 libc heap; each image gets an
 * exact-size decode arena that is freed as soon as the scaled bitmap exists.
 * The DOM and the image/CSS I/O scratch use fixed static arenas.
 */

#include <stdlib.h>
#include <string.h>
#include <ucmd_heap.h>

#include "ucmd_lib.h"
#include "ngui.h"
#include "browser_html.h"
#include "js_browser.h"
#include "uhttp.h"
#include "img.h"
#include "inflate.h"

/* Start wide enough for the desktop nav; smaller screens fall back to
 * 960x640 and 780x540, which still fit a 1024x768 panel. */
#define BR_W            1100
#define BR_H            700
#define BR_TOOLBAR_H    30
#define BR_STATUS_H     18

#define BR_PAGE_MAX     (2u * 1024u * 1024u)
#define BR_IMG_FILE_MAX (512u * 1024u)
#define BR_IMG_PIX_MAX  (8u * 1024u * 1024u)
#define BR_MAX_IMAGES   16
#define BR_IMG_ARENA_MAX (24u * 1024u * 1024u)
#define BR_IMG_LAYOUT_W 1000
#define BR_CSS_MAX      (832u * 1024u)
#define BR_CSS_COMP_MAX (192u * 1024u)
#define BR_SCRIPT_MAX   (512u * 1024u)
#define FETCH_ABORT     (-2)
/* Session cache for remote stylesheets (per launch; cleared at startup). */
#define CSS_CACHE_MAX      16
#define CSS_CACHE_FILE_MAX (256u * 1024u)
#define CSS_CACHE_PREFIX   "bcss-"
#define SCRIPT_CACHE_MAX      16
#define SCRIPT_CACHE_FILE_MAX (512u * 1024u)
#define SCRIPT_CACHE_PREFIX   "bjs-"
#define BR_MAX_SHEETS   6
#define BR_HISTORY      16
#define BR_URL_MAX      512
#define BR_STATUS_MAX   200

#define BR_IMG_RGB565(r, g, b) \
	((unsigned short)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define BR_GRAY   0x8410

enum {
	ID_URL = 1,
	ID_GO,
	ID_BACK,
	ID_FORWARD,
	ID_RELOAD,
	ID_HOME
};

#define ACT_NAV      (1u << 0)
#define ACT_BACK     (1u << 1)
#define ACT_FORWARD  (1u << 2)
#define ACT_RELOAD   (1u << 3)
#define ACT_HOME     (1u << 4)
#define ACT_CLOSE    (1u << 5)

#define ACT_WAIT_NAV (ACT_NAV | ACT_BACK | ACT_FORWARD | ACT_RELOAD | \
		      ACT_HOME | ACT_CLOSE)

struct br_image {
	int state;                  /* 0 empty, 1 ready */
	struct img_image img;       /* scaled bitmap at <= BR_IMG_LAYOUT_W */
	int native_w, native_h;
	char err[32];
};

static struct ngui ui;
static struct ngui_input url_in;
static struct ngui_scroll page_scroll;

static struct br_doc doc;
static struct br_image images[BR_MAX_IMAGES];
static int image_count;

/* Images are loaded one per main-loop iteration so the parsed page stays
 * interactive while a slow network/TLS fetch or 4K decode is in progress. */
static int preload_active;
static int preload_pending;
static int preload_wait;
static unsigned int preload_node;
static int preload_loaded;
static int preload_scroll;
static char post_status_text[BR_STATUS_MAX];
static int post_status_error;
static unsigned int post_status_ttl;

/* Content-relative position/height of each <img>, recorded by the measure
 * pass so image loading can be deferred until it is near the viewport. */
static unsigned short img_top[BR_MAX_NODES];
static unsigned short img_h[BR_MAX_NODES];

/* One reusable I/O buffer: the two halves double as the gzip input scratch
 * for page/CSS fetches, where the heap has no room for another multi-megabyte
 * body.  Decode arenas are allocated per image instead of living here. */
static unsigned char *image_io_buf;
#define image_file_buf (image_io_buf)
#define image_comp_buf (image_io_buf + BR_IMG_FILE_MAX)
#define BR_PAGE_COMP_MAX (2u * BR_IMG_FILE_MAX)
static struct inflate image_inflate_ws;

#if BR_CSS_MAX + BR_CSS_COMP_MAX > 2u * BR_IMG_FILE_MAX
#error "CSS scratch must fit in image_io_buf"
#endif

static char page_url[BR_URL_MAX];
static char page_title[BR_TITLE_MAX];
static char cwd[64];
static int page_is_local;
static int page_loading;

static char status_text[BR_STATUS_MAX];
static unsigned int status_until;
static int status_error;

static char history[BR_HISTORY][BR_URL_MAX];
static int hist_len, hist_pos;

static char click_url[BR_URL_MAX];
static char pending_url[BR_URL_MAX];
static unsigned int pending_action;
static int cancel_fetch;
static int quit;

static struct ngui_rect page_rect;
static unsigned short *page_backbuf;
static int pagebuf_w, pagebuf_h;
static int page_cache_valid;
static int page_cache_scroll;
static unsigned int page_cache_nnode;
static int page_cache_x, page_cache_y, page_cache_w, page_cache_h;
static int layout_valid, layout_left, layout_right;
static unsigned int layout_nnode;
static int layout_content_h;

/* layout output */
struct lay_run {
	unsigned short off, len;
	unsigned short gap;
	unsigned short color;
	unsigned short href;
	char lit[28];
	short img;
	signed short dy;       /* inline image vertical offset */
	unsigned short iw, ih;
	unsigned short w;
	unsigned char font_px, bold, italic, mono, underline;
	unsigned char has_lit;
};

struct lay {
	int draw;
	struct ngui *ui;
	int left, right;
	int y;
	int line_x, line_y, line_h;
	int run_x;
	int align;
	int depth;
	int space_pending;
	struct lay_run runs[96];
	int nruns;
};

struct hit {
	int x, y, w, h;
	unsigned short node;
};

#define HIT_MAX 96
static struct hit hits[HIT_MAX];
static int nhits;

static unsigned short block_h[BR_MAX_NODES];
static char li_marker[BR_MAX_NODES][6];

/* one connection object; the browser fetches one resource at a time */

/* ---- tiny strings ---- */

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

static void s_append(char *d, unsigned int *n, const char *s, unsigned int cap)
{
	while (s && *s && *n + 1 < cap)
		d[(*n)++] = *s++;
	d[*n] = '\0';
}

static void s_append_dec(char *d, unsigned int *n, unsigned int v,
			 unsigned int cap)
{
	char tmp[12];
	unsigned int i = 0, j;

	do {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < sizeof(tmp));
	for (j = 0; j < i && *n + 1 < cap; j++)
		d[(*n)++] = tmp[i - 1 - j];
	d[*n] = '\0';
}

static int s_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

static int starts_with(const char *s, const char *p)
{
	unsigned int i = 0;

	while (p[i]) {
		if (s[i] != p[i])
			return 0;
		i++;
	}
	return 1;
}

static int contains_ci(const char *s, const char *p)
{
	unsigned int i, j;

	for (i = 0; s[i]; i++) {
		for (j = 0; p[j]; j++) {
			char a = s[i + j];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (a != p[j])
				break;
		}
		if (!p[j])
			return 1;
	}
	return 0;
}

static char *trim(char *s)
{
	unsigned int n;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	n = s_len(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
		     s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = '\0';
	return s;
}

/* ---- per-process heap ---- */

static int heap_start(void)
{
	image_io_buf = (unsigned char *)malloc(2u * BR_IMG_FILE_MAX);
	if (!image_io_buf)
		return -1;
	page_backbuf = (unsigned short *)malloc(
		(unsigned int)ui.w * (unsigned int)ui.h * 2u);
	pagebuf_w = page_backbuf ? ui.w : 0;
	pagebuf_h = page_backbuf ? ui.h : 0;
	page_cache_valid = 0;
	return 0;
}

static void heap_clear(void)
{
	ngui_font_detach(&ui);
	heap_reset();
	image_io_buf = 0;
	page_backbuf = 0;
	pagebuf_w = pagebuf_h = 0;
	page_cache_valid = 0;
}

static void browser_rules_alloc(void)
{
	struct br_rule *rules;

	rules = (struct br_rule *)malloc(
		(unsigned int)(BR_MAX_RULES * sizeof(struct br_rule)));
	br_set_rule_store(&doc, rules, rules ? BR_MAX_RULES : 0);
}

/* ---- status ---- */

static void set_status(const char *s, int err, unsigned int ttl_ms)
{
	s_copy(status_text, s, sizeof(status_text));
	status_error = err;
	status_until = ttl_ms ? u_ticks_ms() + ttl_ms : 0;
	ngui_invalidate(&ui);
}

/* ---- JavaScript ---- */

static int resolve_link(const char *base, const char *href, char *out,
			unsigned int cap);

static char js_last_status[BR_STATUS_MAX];
static int js_last_error;

static void js_host_status(const char *msg, int error)
{
	s_copy(js_last_status, msg, sizeof(js_last_status));
	js_last_error = error;
	set_status(msg, error, 6000);
}

static void js_host_report(const char *msg)
{
	u_puts("[js] ");
	u_puts(msg);
	u_putc('\n');
	s_copy(js_last_status, msg, sizeof(js_last_status));
	js_last_error = 1;
	set_status(msg, 1, 8000);
}

static void js_host_navigate(const char *url)
{
	char resolved[BR_URL_MAX];

	if (!url || !url[0])
		return;
	if (resolve_link(page_url, url, resolved, sizeof(resolved)) != 0)
		return;
	s_copy(pending_url, resolved, sizeof(pending_url));
	pending_action |= ACT_NAV;
	cancel_fetch = 1;
}

static void js_host_changed(void)
{
	/* Class/style/attribute mutations change computed styles, not just the
	 * text: drop the per-node style cache before the next layout. */
	br_reset_styles(&doc);
	layout_valid = 0;
	ngui_invalidate(&ui);
}

static int fetch_script_text(const char *url, unsigned char **out,
			     unsigned int *outlen, char *err,
			     unsigned int errcap);

static void script_error(const char *what, const char *url, const char *why)
{
	char msg[192];
	unsigned int n = 0;

	s_append(msg, &n, what, sizeof(msg));
	if (url && url[0]) {
		s_append(msg, &n, " ", sizeof(msg));
		s_append(msg, &n, url, sizeof(msg));
	}
	if (why && why[0]) {
		s_append(msg, &n, ": ", sizeof(msg));
		s_append(msg, &n, why, sizeof(msg));
	}
	js_host_report(msg);
}

/* Run the page's scripts in document order: external <script src> bodies are
 * fetched, inline bodies are copied because MuJS needs NUL-terminated text. */
static void run_page_scripts(void)
{
	struct js_browser_host host;
	unsigned int i;

	if (!doc.nscripts)
		return;
	host.status = js_host_status;
	host.report = js_host_report;
	host.navigate = js_host_navigate;
	host.changed = js_host_changed;
	host.page_url = page_url;
	host.width = ui.w;
	host.height = ui.h;
	if (js_browser_start(&doc, &host) != 0) {
		js_host_status("JavaScript: out of memory", 1);
		return;
	}
	for (i = 0; i < doc.nscripts; i++) {
		struct br_script *script = &doc.scripts[i];

		if (script->src_off) {
			const char *src_attr = doc.attrs + script->src_off;
			char url[BR_URL_MAX];
			unsigned char *data;
			unsigned int len;
			char err[64];
			int rv;

			if (!src_attr[0])
				continue;
			if (resolve_link(page_url, src_attr, url,
					 sizeof(url)) != 0) {
				script_error("Cannot load script", src_attr,
					     "unsupported URL");
				continue;
			}
			set_status("Loading script...", 0, 0);
			rv = fetch_script_text(url, &data, &len, err,
					       sizeof(err));
			if (rv == FETCH_ABORT)
				break;
			if (rv != 0) {
				script_error("Cannot load script", url, err);
				continue;
			}
			js_browser_run((const char *)data, url);
			free(data);
		} else {
			unsigned int off = script->off;
			unsigned int len = script->len;
			unsigned int k;
			char *src = (char *)malloc(len + 1);

			if (!src) {
				js_host_status("JavaScript: out of memory", 1);
				break;
			}
			for (k = 0; k < len; k++)
				src[k] = doc.script_src[off + k];
			src[len] = '\0';
			js_browser_run(src, page_url);
			free(src);
		}
		if (pending_action)
			break;
	}
	/* Keep the state alive for setTimeout/ready callbacks; load_page() and
	 * load_home() stop it before the next navigation. */
	if (doc.scripts_truncated && !js_last_error)
		js_host_status("Page has more than 32 scripts; extra scripts skipped",
			       1);
}

/* ---- images ---- */

static void image_reset(void)
{
	int i;

	for (i = 0; i < BR_MAX_IMAGES; i++) {
		images[i].state = 0;
		images[i].native_w = images[i].native_h = 0;
		images[i].err[0] = '\0';
	}
	image_count = 0;
	preload_active = 0;
	preload_pending = 0;
	preload_wait = 0;
	preload_node = 0;
	preload_loaded = 0;
}

/* Decode into the shared arena, then copy/scale the result into a display
 * bitmap of at most BR_IMG_LAYOUT_W pixels wide.  The arena is reused by the
 * next image, so the per-image cost is its scaled bitmap, not full 4K RGBA
 * pixels.  Transparent edges are composited over white. */
static int decode_image_data(const unsigned char *data, unsigned int len,
			     struct br_image *bi)
{
	struct img_info info;
	struct img_options opt;
	struct img_mem mem;
	struct img_image decoded;
	unsigned char *arena;
	unsigned int est, bytes, i;
	unsigned short *px;
	unsigned char *alpha = 0;
	int dw, dh, r;

	if (img_sniff(data, len, &info) != 0) {
		s_copy(bi->err, "unknown image format", sizeof(bi->err));
		return -1;
	}
	est = img_estimate(&info);
	if (est == 0xFFFFFFFFu || est > BR_IMG_ARENA_MAX) {
		s_copy(bi->err, "image too large", sizeof(bi->err));
		return -1;
	}
	arena = (unsigned char *)malloc(est);
	if (!arena) {
		s_copy(bi->err, "out of memory", sizeof(bi->err));
		return -1;
	}
	img_mem_init(&mem, arena, est);
	opt.max_pixels = BR_IMG_PIX_MAX;
	r = img_decode(data, len, &opt, &mem, &decoded);
	if (r != 0) {
		s_copy(bi->err, img_error_string(r), sizeof(bi->err));
		goto fail;
	}
	if (decoded.w == 0 || decoded.h == 0) {
		s_copy(bi->err, "empty image", sizeof(bi->err));
		goto fail;
	}
	bi->native_w = (int)decoded.w;
	bi->native_h = (int)decoded.h;

	dw = (int)decoded.w;
	dh = (int)decoded.h;
	if (dw > BR_IMG_LAYOUT_W) {
		dh = (int)((unsigned int)decoded.h * BR_IMG_LAYOUT_W /
			   decoded.w);
		dw = BR_IMG_LAYOUT_W;
	}
	if (dh < 1)
		dh = 1;
	if (dw < 1)
		dw = 1;
	bytes = (unsigned int)dw * (unsigned int)dh * 2u;
	px = (unsigned short *)malloc(bytes);
	if (!px) {
		s_copy(bi->err, "out of memory", sizeof(bi->err));
		goto fail;
	}
	/* Keep the alpha plane for the scaled image so transparent logos and
	 * icons composite over the page instead of the browser's dark theme
	 * background.  Very large alpha images stay flattened to bound the
	 * heap use. */
	if (decoded.alpha && (unsigned int)dw * (unsigned int)dh <= (1u << 20)) {
		alpha = (unsigned char *)malloc((unsigned int)dw *
						(unsigned int)dh);
		if (alpha) {
			for (i = 0; i < (unsigned int)dw * (unsigned int)dh; i++)
				alpha[i] = 255;
		}
	}
	for (i = 0; i < (unsigned int)dw * (unsigned int)dh; i++)
		px[i] = ui.theme.bg;

	{
		struct img_blit b;

		b.dst = px;
		b.dst_w = dw;
		b.dst_h = dh;
		b.clip_x = 0;
		b.clip_y = 0;
		b.clip_w = dw;
		b.clip_h = dh;
		b.x = 0;
		b.y = 0;
		b.scale = ((unsigned int)dw << 16) / decoded.w;
		b.bg = ui.theme.bg;
		b.bg2 = ui.theme.bg;
		b.dst_a = alpha;
		b.flags = alpha ? IMG_BLIT_SAVE_ALPHA : IMG_BLIT_OVER_DST;
		img_draw(&decoded, &b);
	}

	bi->img.format = decoded.format;
	bi->img.w = (unsigned int)dw;
	bi->img.h = (unsigned int)dh;
	bi->img.px = px;
	bi->img.alpha = alpha;
	bi->img.flags = 0;
	bi->state = 1;
	free(arena);
	return 0;

fail:
	free(arena);
	return -1;
}

/* ---- URL / local path handling ---- */

static int is_http(const char *s)
{
	return starts_with(s, "http://") || starts_with(s, "https://");
}

/* Collapse "/./", "/../" and duplicate slashes in an absolute path. */
static int normalize_abs(const char *in, char *out, unsigned int cap)
{
	char seg[64];
	unsigned int i = 0, o = 1;
	int depth = 0;
	int trailing;

	if (!in || in[0] != '/' || cap < 2)
		return -1;
	trailing = 0;
	{
		unsigned int n = s_len(in);
		if (n > 1 && in[n - 1] == '/')
			trailing = 1;
	}
	out[0] = '/';
	while (in[i]) {
		unsigned int n = 0;

		while (in[i] == '/')
			i++;
		while (in[i] && in[i] != '/' && in[i] != '?' && in[i] != '#') {
			if (n + 1 < sizeof(seg))
				seg[n++] = in[i];
			i++;
		}
		seg[n] = '\0';
		if (n == 0)
			break;
		if (s_eq(seg, "."))
			continue;
		if (s_eq(seg, "..")) {
			if (depth > 0) {
				while (o > 1 && out[o - 1] != '/')
					o--;
				if (o > 1)
					o--;
				depth--;
			}
			continue;
		}
		if (o > 1) {
			if (o + 1 >= cap)
				return -1;
			out[o++] = '/';
		}
		if (o + n + 1 >= cap)
			return -1;
		{
			unsigned int j;
			for (j = 0; j < n; j++)
				out[o++] = seg[j];
		}
		depth++;
	}
	if (trailing && o > 1) {
		if (o + 1 >= cap)
			return -1;
		out[o++] = '/';
	}
	out[o] = '\0';
	return 0;
}

static void make_abs_local(const char *in, char *out, unsigned int cap)
{
	char joined[BR_URL_MAX];

	if (!in || !in[0]) {
		s_copy(out, cwd[0] ? cwd : "/", cap);
		return;
	}
	if (in[0] == '/') {
		if (normalize_abs(in, out, cap) != 0)
			s_copy(out, in, cap);
		return;
	}
	{
		unsigned int n = 0;
		s_append(joined, &n, cwd[0] ? cwd : "/", sizeof(joined));
		s_append(joined, &n, "/", sizeof(joined));
		s_append(joined, &n, in, sizeof(joined));
	}
	if (normalize_abs(joined, out, cap) != 0)
		s_copy(out, joined, cap);
}

/* ---- canonical origin cache ---- *
 *
 * Sites commonly redirect http://host to https://host and then to
 * https://www.host.  Remember the final scheme/host of the last navigation so
 * a bare domain typed again goes straight there instead of paying for two
 * more connections and TLS handshakes.  Only default-port requests are
 * rewritten; explicit ports are left alone. */
#define ORIGIN_CACHE_MAX 8
#define ORIGIN_FILE      "/share/borigins"
#define ORIGIN_BUF_SIZE  (ORIGIN_CACHE_MAX * (UHTTP_HOST_MAX + 16 + \
			   UHTTP_HOST_MAX + 2))

struct origin_cache_entry {
	char host[UHTTP_HOST_MAX];
	char origin[UHTTP_HOST_MAX + 16];
};
static struct origin_cache_entry origins[ORIGIN_CACHE_MAX];
static int origin_count;
static char origin_buf[ORIGIN_BUF_SIZE];

static int default_port(const struct uhttp_url *u)
{
	return u->secure ? u->port == 443 : u->port == 80;
}

static void url_origin(const struct uhttp_url *u, char *out,
		       unsigned int cap)
{
	unsigned int n = 0;

	s_append(out, &n, u->secure ? "https://" : "http://", cap);
	s_append(out, &n, u->host, cap);
	if (!default_port(u)) {
		s_append(out, &n, ":", cap);
		s_append_dec(out, &n, u->port, cap);
	}
}

static void origin_cache_apply(char *url, unsigned int cap)
{
	struct uhttp_url u;
	char out[BR_URL_MAX];
	unsigned int n = 0;
	int i;

	if (uhttp_parse_url(url, &u) != 0 || !default_port(&u))
		return;
	for (i = 0; i < origin_count; i++) {
		if (!s_eq(u.host, origins[i].host))
			continue;
		s_append(out, &n, origins[i].origin, sizeof(out));
		s_append(out, &n, u.path, sizeof(out));
		s_copy(url, out, cap);
		return;
	}
}

static void origin_cache_save(void)
{
	unsigned int n = 0;
	int i;

	for (i = 0; i < origin_count; i++) {
		s_append(origin_buf, &n, origins[i].host, sizeof(origin_buf));
		s_append(origin_buf, &n, " ", sizeof(origin_buf));
		s_append(origin_buf, &n, origins[i].origin, sizeof(origin_buf));
		s_append(origin_buf, &n, "\n", sizeof(origin_buf));
	}
	if (n)
		u_syscall3(SYS_WRITE_FILE, (int)ORIGIN_FILE,
			   (int)origin_buf, (int)n);
}

static void origin_cache_load(void)
{
	char *p;
	int rn;

	rn = u_syscall3(SYS_READ_FILE, (int)ORIGIN_FILE, (int)origin_buf,
			(int)(sizeof(origin_buf) - 1));
	if (rn <= 0)
		return;
	origin_buf[rn] = '\0';
	p = origin_buf;
	while (*p && origin_count < ORIGIN_CACHE_MAX) {
		char *line = p, *origin;

		while (*p && *p != '\n')
			p++;
		if (*p)
			*p++ = '\0';
		if (!line[0])
			continue;
		origin = line;
		while (*origin && *origin != ' ')
			origin++;
		if (!*origin)
			continue;
		*origin++ = '\0';
		if (!line[0] || !is_http(origin))
			continue;
		{
			struct uhttp_url uo;

			if (uhttp_parse_url(origin, &uo) != 0 ||
			    !s_eq(uo.path, "/"))
				continue;
		}
		s_copy(origins[origin_count].host, line,
		       sizeof(origins[origin_count].host));
		s_copy(origins[origin_count].origin, origin,
		       sizeof(origins[origin_count].origin));
		origin_count++;
	}
}

static int host_is_www_of(const char *host, const char *base)
{
	return host[0] == 'w' && host[1] == 'w' && host[2] == 'w' &&
	       host[3] == '.' && s_eq(host + 4, base);
}

static void origin_cache_learn(const char *requested, const char *final)
{
	struct uhttp_url from, to;
	char origin[UHTTP_HOST_MAX + 16];
	int i;

	if (!is_http(requested) || !is_http(final))
		return;
	if (uhttp_parse_url(requested, &from) != 0 ||
	    uhttp_parse_url(final, &to) != 0)
		return;
	if (!default_port(&from))
		return;
	if (s_eq(from.host, to.host) && from.secure == to.secure &&
	    from.port == to.port)
		return;
	/* Only remember origin-level canonicalisation: the same path, and
	 * either the same host or its www./apex sibling.  A path- or
	 * host-specific redirect (a shortener, a login page) must not poison
	 * every request to the typed host. */
	if (!s_eq(from.path, to.path))
		return;
	if (!s_eq(from.host, to.host) &&
	    !host_is_www_of(to.host, from.host) &&
	    !host_is_www_of(from.host, to.host))
		return;
	url_origin(&to, origin, sizeof(origin));
	for (i = 0; i < origin_count; i++) {
		if (s_eq(origins[i].host, from.host)) {
			if (!s_eq(origins[i].origin, origin)) {
				s_copy(origins[i].origin, origin,
				       sizeof(origins[i].origin));
				origin_cache_save();
			}
			return;
		}
	}
	i = origin_count < ORIGIN_CACHE_MAX ? origin_count++ : 0;
	s_copy(origins[i].host, from.host, sizeof(origins[i].host));
	s_copy(origins[i].origin, origin, sizeof(origins[i].origin));
	origin_cache_save();
}

static int path_dir(const char *path, char *out, unsigned int cap)
{
	unsigned int i, last = 0;

	for (i = 0; path[i]; i++)
		if (path[i] == '/')
			last = i;
	if (last == 0) {
		if (cap < 2)
			return -1;
		out[0] = '/';
		out[1] = '\0';
		return 0;
	}
	if (last + 1 >= cap)
		return -1;
	{
		unsigned int j;
		for (j = 0; j < last; j++)
			out[j] = path[j];
		out[last] = '\0';
	}
	return 0;
}

/* Resolve an href against the current page. 0 = ok, -1 = ignore. */
static int resolve_link(const char *base, const char *href, char *out,
			unsigned int cap)
{
	char joined[BR_URL_MAX];

	if (!href || !href[0] || href[0] == '#')
		return -1;
	if (starts_with(href, "mailto:") || starts_with(href, "javascript:") ||
	    starts_with(href, "data:") || starts_with(href, "tel:"))
		return -1;
	if (starts_with(href, "about:")) {
		s_copy(out, href, cap);
		return 0;
	}
	if (is_http(href)) {
		s_copy(out, href, cap);
		return 0;
	}
	if (starts_with(href, "//")) {
		if (is_http(base)) {
			unsigned int n = 0;
			s_append(out, &n, starts_with(base, "https:") ?
				 "https:" : "http:", cap);
			s_append(out, &n, href, cap);
			return 0;
		}
		return -1;
	}
	if (is_http(base)) {
		return uhttp_resolve(base, href, out, cap) == 0 ? 0 : -1;
	}
	/* local page */
	if (href[0] == '/') {
		if (normalize_abs(href, out, cap) != 0)
			return -1;
		return 0;
	}
	{
		char dir[BR_URL_MAX];
		unsigned int n = 0;

		if (s_eq(base, "about:home"))
			s_copy(dir, cwd[0] ? cwd : "/", sizeof(dir));
		else if (path_dir(base, dir, sizeof(dir)) != 0)
			s_copy(dir, "/", sizeof(dir));
		s_append(joined, &n, dir, sizeof(joined));
		if (n == 0 || joined[n - 1] != '/')
			s_append(joined, &n, "/", sizeof(joined));
		s_append(joined, &n, href, sizeof(joined));
	}
	return normalize_abs(joined, out, cap) == 0 ? 0 : -1;
}

/* Best-effort decompression: keep the first `cap` bytes and discard the
 * rest instead of failing when a page or stylesheet is larger than the
 * buffer.  The stream is still walked to the end, so a normal page is
 * verified; a truncated compressed input still returns whatever was decoded
 * so far. */
struct trunc_buf {
	unsigned char *dst;
	unsigned int cap, pos;
	int full;
};

static int trunc_write(void *ctx, const unsigned char *data, unsigned int len)
{
	struct trunc_buf *m = (struct trunc_buf *)ctx;

	if (len > m->cap - m->pos) {
		unsigned int room = m->cap - m->pos;

		if (room) {
			unsigned int i;

			for (i = 0; i < room; i++)
				m->dst[m->pos + i] = data[i];
			m->pos += room;
		}
		m->full = 1;
		return 0;
	}
	{
		unsigned int i;

		for (i = 0; i < len; i++)
			m->dst[m->pos + i] = data[i];
	}
	m->pos += len;
	return 0;
}

static void worker_wait_ui(void);

static void inflate_ui_pump(void *ctx)
{
	static unsigned int last;
	unsigned int now = u_ticks_ms();

	(void)ctx;
	if ((int)(now - last) >= 50) {
		last = now;
		worker_wait_ui();
	}
}

static int inflate_trunc(const unsigned char *src, unsigned int len,
			 unsigned char *dst, unsigned int cap,
			 unsigned int *outlen, int gzip, int *truncated)
{
	struct inflate_src in;
	struct inflate_sink sink;
	struct trunc_buf m;
	unsigned int got = 0;
	int r;

	in.p = src;
	in.len = len;
	in.more = 0;
	in.ctx = 0;
	m.dst = dst;
	m.cap = cap;
	m.pos = 0;
	m.full = 0;
	sink.write = trunc_write;
	sink.ctx = &m;
	inflate_init(&image_inflate_ws, &in, &sink);
	image_inflate_ws.progress = inflate_ui_pump;
	image_inflate_ws.progress_ctx = 0;
	r = gzip ? inflate_gzip(&image_inflate_ws, &got) :
		   inflate_zlib(&image_inflate_ws, &got);
	*outlen = m.pos;
	*truncated = m.full || r != 0;
	if (r != 0 && m.pos == 0)
		return -1;
	return 0;
}

/* ---- headless fetch helper ------------------------------------------- *
 *
 * Network I/O runs in a separate ring-3 process (`curl __fetch`): it writes
 * the raw response and a small metadata file under /share.  While it works,
 * the browser keeps drawing and scrolling its own window, so a slow DNS/TLS
 * handshake or a multi-megabyte body never freezes the UI.
 */

#define FETCH_REQ  "/share/bfreq"
#define FETCH_BODY "/share/bfbody"
#define FETCH_META "/share/bfmeta"
#define FETCH_DONE "/share/bfdone"

struct fetch_meta {
	int status;
	int error;
	int trunc;
	unsigned int len;
	char encoding[16];
	char ctype[64];
	char final[BR_URL_MAX];
};

static void draw(unsigned int *action);
static void handle_keys(unsigned int *action);
static void handle_page_click(unsigned int *action);
static int fetch_helper_tty = -2;
static int fetch_helper_pid = -1;
static unsigned int fetch_seq;
static struct fetch_meta fetch_meta;

static unsigned int meta_uint(char **pp)
{
	char *p = *pp;
	unsigned int v = 0;

	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (unsigned int)(*p - '0');
		p++;
	}
	while (*p && *p != '\n')
		p++;
	if (*p == '\n')
		p++;
	*pp = p;
	return v;
}

static void meta_str(char **pp, char *out, unsigned int cap)
{
	char *p = *pp;
	unsigned int n = 0;

	while (*p && *p != '\n' && n + 1 < cap)
		out[n++] = *p++;
	out[n] = '\0';
	while (*p && *p != '\n')
		p++;
	if (*p == '\n')
		p++;
	*pp = p;
}

static void worker_wait_ui(void)
{
	unsigned int action = 0;

	if (!ngui_begin(&ui))
		return;
	draw(&action);
	handle_keys(&action);
	handle_page_click(&action);
	if (action & ACT_WAIT_NAV) {
		pending_action |= action & ACT_WAIT_NAV;
		if ((action & ACT_NAV) && click_url[0])
			s_copy(pending_url, click_url, sizeof(pending_url));
		click_url[0] = '\0';
		cancel_fetch = 1;
	}
	if (!ngui_focus(&ui) && ngui_scroll_keys(&ui, &page_scroll))
		ngui_invalidate(&ui);
	ngui_end(&ui);
}

static void fetch_helper_stop(void)
{
	int i;

	if (fetch_helper_pid > 0) {
		u_kill((unsigned int)fetch_helper_pid);
		for (i = 0; i < 40 && u_alive((unsigned int)fetch_helper_pid); i++)
			u_sleep_ms(5);
		/* Let the reaper release the NIC before the next request. */
		u_sleep_ms(10);
	}
	fetch_helper_pid = -1;
}

static int fetch_helper_start(char *err, unsigned int errcap)
{
	if (fetch_helper_pid > 0 &&
	    u_alive((unsigned int)fetch_helper_pid))
		return 0;
	fetch_helper_pid = -1;
	if (fetch_helper_tty == -2) {
		int t = u_tty_create();

		fetch_helper_tty = t > 0 ? t : 0;
	}
	fetch_helper_pid = u_spawn("/ramfs/bin/curl", "__serve",
				   fetch_helper_tty, "/");
	if (fetch_helper_pid <= 0) {
		fetch_helper_pid = -1;
		s_copy(err, "cannot start network helper", errcap);
		return -1;
	}
	return 0;
}

static int worker_fetch(const char *url, unsigned int cap,
			struct fetch_meta *m, char *err, unsigned int errcap)
{
	static char req[768], meta[1024], done[24];
	char *p;
	unsigned int n = 0, deadline;
	int rn, mseq, retried = 0;
	struct sys_stat st;

	if (cancel_fetch || pending_action)
		return FETCH_ABORT;
	if (u_stat("/share", &st) != 0 || !st.is_dir ||
	    u_stat("/ramfs/bin/curl", &st) != 0) {
		s_copy(err, "network helper unavailable", errcap);
		return -1;
	}
	fetch_seq++;
	if (!fetch_seq)
		fetch_seq = 1;
	s_append_dec(req, &n, fetch_seq, sizeof(req));
	s_append(req, &n, " ", sizeof(req));
	s_append_dec(req, &n, cap, sizeof(req));
	s_append(req, &n, " ", sizeof(req));
	s_append(req, &n, url, sizeof(req));
	s_append(req, &n, "\n", sizeof(req));
	u_syscall1(SYS_UNLINK, (int)FETCH_DONE);
	u_syscall1(SYS_UNLINK, (int)FETCH_META);
	if (u_syscall3(SYS_WRITE_FILE, (int)FETCH_REQ, (int)req, (int)n) < 0) {
		s_copy(err, "cannot queue network request", errcap);
		return -1;
	}
	if (fetch_helper_start(err, errcap) != 0)
		return -1;
	deadline = u_ticks_ms() + UHTTP_TOTAL_TIMEOUT_MS + 5000;
	for (;;) {
		worker_wait_ui();
		if (cancel_fetch || pending_action) {
			fetch_helper_stop();
			return FETCH_ABORT;
		}
		rn = u_syscall3(SYS_READ_FILE, (int)FETCH_DONE, (int)done,
				(int)(sizeof(done) - 1));
		if (rn > 0) {
			done[rn] = '\0';
			p = done;
			mseq = (int)meta_uint(&p);
			if (mseq == (int)fetch_seq)
				break;
		}
		if (!u_alive((unsigned int)fetch_helper_pid)) {
			fetch_helper_pid = -1;
			if (!retried && fetch_helper_start(err, errcap) == 0) {
				retried = 1;
				continue;
			}
			s_copy(err, "network helper stopped", errcap);
			return -1;
		}
		if ((int)(u_ticks_ms() - deadline) >= 0) {
			fetch_helper_stop();
			s_copy(err, "network request timed out", errcap);
			return -1;
		}
		u_sleep_ms(20);
	}
	if (cancel_fetch || pending_action)
		return FETCH_ABORT;
	rn = u_syscall3(SYS_READ_FILE, (int)FETCH_META, (int)meta,
			(int)(sizeof(meta) - 1));
	if (rn <= 0) {
		s_copy(err, "network helper failed", errcap);
		return -1;
	}
	meta[rn] = '\0';
	p = meta;
	mseq = (int)meta_uint(&p);
	if (mseq != (int)fetch_seq) {
		s_copy(err, "stale network response", errcap);
		return -1;
	}
	m->status = (int)meta_uint(&p);
	m->error = (int)meta_uint(&p);
	m->trunc = (int)meta_uint(&p);
	meta_str(&p, m->encoding, sizeof(m->encoding));
	meta_str(&p, m->ctype, sizeof(m->ctype));
	meta_str(&p, m->final, sizeof(m->final));
	m->len = meta_uint(&p);
	if (m->error) {
		s_copy(err, uhttp_error_string(m->error), errcap);
		return -1;
	}
	return 0;
}

static int fetch_remote(const char *url, unsigned int cap,
			unsigned char *plain, unsigned int plaincap,
			unsigned char *comp, unsigned int compcap,
			struct fetch_meta *m, int *truncated,
			char *err, unsigned int errcap)
{
	int rv, zr, was_trunc;

	rv = worker_fetch(url, cap, m, err, errcap);
	if (rv == FETCH_ABORT)
		return FETCH_ABORT;
	if (rv != 0)
		return -1;
	if (contains_ci(m->encoding, "gzip") ||
	    contains_ci(m->encoding, "deflate")) {
		unsigned int comp_len = 0, plen = 0;

		rv = u_syscall3(SYS_READ_FILE, (int)FETCH_BODY, (int)comp,
				(int)compcap);
		if (rv < 0) {
			s_copy(err, "empty response", errcap);
			return -1;
		}
		comp_len = (unsigned int)rv;
		if (comp_len < m->len) {
			s_copy(err, "compressed response is too large", errcap);
			return -1;
		}
		if (contains_ci(m->encoding, "deflate"))
			zr = inflate_trunc(comp, comp_len, plain, plaincap,
					   &plen, 0, &was_trunc);
		else
			zr = inflate_trunc(comp, comp_len, plain, plaincap,
					   &plen, 1, &was_trunc);
		if (zr != 0) {
			s_copy(err, "compressed response is corrupt", errcap);
			return -1;
		}
		*truncated = m->trunc || was_trunc;
		return (cancel_fetch || pending_action) ? FETCH_ABORT :
			(int)plen;
	}
	rv = u_syscall3(SYS_READ_FILE, (int)FETCH_BODY, (int)plain,
			(int)plaincap);
	if (rv < 0) {
		s_copy(err, "empty response", errcap);
		return -1;
	}
	*truncated = m->trunc || ((unsigned int)rv < m->len);
	return (cancel_fetch || pending_action) ? FETCH_ABORT : rv;
}

/* ---- HTTP / file fetch ---- */

static int read_url(const char *url, unsigned int cap, unsigned char **out,
		    unsigned int *outlen, int *is_local, char *ctype,
		    unsigned int ctcap, char *final, unsigned int finalcap,
		    char *err, unsigned int errcap, int *truncated)
{
	unsigned char *buf;
	unsigned int len;
	int tr = 0;
	int rv;

	*out = 0;
	*outlen = 0;
	*truncated = 0;
	if (final && finalcap)
		final[0] = '\0';
	if (ctype && ctcap)
		ctype[0] = '\0';

	if (url[0] == '/') {
		buf = (unsigned char *)malloc(cap);
		if (!buf) {
			s_copy(err, "out of memory", errcap);
			return -1;
		}
		rv = u_syscall3(SYS_READ_FILE, (int)url, (int)buf, (int)cap);
		if (rv < 0) {
			free(buf);
			s_copy(err, "file not found", errcap);
			return -1;
		}
		*out = buf;
		*outlen = (unsigned int)rv;
		*is_local = 1;
		if (ctype)
			s_copy(ctype, "text/html", ctcap);
		if (final)
			s_copy(final, url, finalcap);
		if ((unsigned int)rv == cap)
			*truncated = 1;
		return 0;
	}

	buf = (unsigned char *)malloc(cap);
	if (!buf) {
		s_copy(err, "out of memory", errcap);
		return -1;
	}
	rv = fetch_remote(url, cap, buf, cap, image_io_buf, BR_PAGE_COMP_MAX,
			  &fetch_meta, &tr, err, errcap);
	if (rv == FETCH_ABORT) {
		free(buf);
		return FETCH_ABORT;
	}
	if (rv < 0) {
		free(buf);
		return -1;
	}
	len = (unsigned int)rv;
	if (len == 0) {
		free(buf);
		s_copy(err, "empty response", errcap);
		return -1;
	}
	if (ctype)
		s_copy(ctype, fetch_meta.ctype, ctcap);
	if (final && fetch_meta.final[0])
		s_copy(final, fetch_meta.final, finalcap);
	*is_local = 0;
	*out = buf;
	*outlen = len;
	*truncated = tr;
	return 0;
}

/* Remote scripts are cached under /share for the current boot, like the
 * stylesheet cache, so reloads and later visits skip the network. */
static int script_cache_count;

static unsigned int script_cache_hash(const char *s)
{
	unsigned int h = 2166136261u;

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static void script_cache_path(const char *url, char *out, unsigned int cap)
{
	unsigned int n = 0;

	s_append(out, &n, "/share/" SCRIPT_CACHE_PREFIX, cap);
	s_append_dec(out, &n, script_cache_hash(url), cap);
}

static void script_cache_clear(void)
{
	struct sys_dirent d;
	char path[96];

	for (;;) {
		int i, found = 0;

		for (i = 0; u_syscall3(SYS_LIST_DIR, (int)"/share", i,
				       (int)&d) == 1; i++) {
			unsigned int n = 0;

			if (!starts_with(d.name, SCRIPT_CACHE_PREFIX))
				continue;
			s_append(path, &n, "/share/", sizeof(path));
			s_append(path, &n, d.name, sizeof(path));
			if (u_syscall1(SYS_UNLINK, (int)path) < 0)
				return;
			found = 1;
			break;
		}
		if (!found)
			break;
	}
	script_cache_count = 0;
}

static void script_cache_init(void)
{
	struct sys_dirent d;
	int i, count = 0;

	for (i = 0; u_syscall3(SYS_LIST_DIR, (int)"/share", i,
			       (int)&d) == 1; i++)
		if (starts_with(d.name, SCRIPT_CACHE_PREFIX))
			count++;
	if (count >= SCRIPT_CACHE_MAX) {
		script_cache_clear();
		return;
	}
	script_cache_count = count;
}

static int script_cache_get(const char *url, unsigned char **out,
			    unsigned int *outlen)
{
	char path[96];
	struct sys_stat st;
	unsigned char *buf;
	int rn;

	script_cache_path(url, path, sizeof(path));
	if (u_stat(path, &st) != 0 || st.is_dir || st.size == 0 ||
	    st.size >= SCRIPT_CACHE_FILE_MAX)
		return 0;
	buf = (unsigned char *)malloc(st.size + 1);
	if (!buf)
		return 0;
	rn = u_syscall3(SYS_READ_FILE, (int)path, (int)buf, (int)st.size);
	if (rn != (int)st.size) {
		free(buf);
		return 0;
	}
	buf[st.size] = '\0';
	*out = buf;
	*outlen = st.size;
	return 1;
}

static void script_cache_put(const char *url, const unsigned char *data,
			     unsigned int len)
{
	char path[96];

	if (script_cache_count >= SCRIPT_CACHE_MAX || len == 0 ||
	    len >= SCRIPT_CACHE_FILE_MAX)
		return;
	script_cache_path(url, path, sizeof(path));
	if (u_syscall3(SYS_WRITE_FILE, (int)path, (int)data,
		       (int)len) >= 0)
		script_cache_count++;
}

/* Fetch one external script into a heap buffer and NUL-terminate it. */
static int fetch_script_text(const char *url, unsigned char **out,
			     unsigned int *outlen, char *err,
			     unsigned int errcap)
{
	unsigned char *buf;
	unsigned int len;
	int local, truncated = 0;
	char ctype[64], final[BR_URL_MAX];
	int rv;

	*out = 0;
	*outlen = 0;
	if (url[0] != '/' && script_cache_get(url, out, outlen))
		return 0;
	rv = read_url(url, BR_SCRIPT_MAX, &buf, &len, &local, ctype,
		      sizeof(ctype), final, sizeof(final), err, errcap,
		      &truncated);
	if (rv != 0)
		return rv;
	if (truncated || len >= BR_SCRIPT_MAX) {
		free(buf);
		s_copy(err, "script is too large", errcap);
		return -1;
	}
	buf[len] = '\0';
	if (!local)
		script_cache_put(url, buf, len);
	*out = buf;
	*outlen = len;
	return 0;
}

/* Fetch an image into the fixed scratch buffers.  The scaled bitmaps live on
 * the heap, so images must not allocate their file body from it as well. */
static int fetch_image(const char *url, unsigned int cap,
		       unsigned char **out, unsigned int *outlen,
		       char *err, unsigned int errcap)
{
	unsigned int len;
	int tr = 0;
	int rv;

	*out = 0;
	*outlen = 0;
	if (cap > BR_IMG_FILE_MAX)
		cap = BR_IMG_FILE_MAX;
	if (url[0] == '/') {
		rv = u_syscall3(SYS_READ_FILE, (int)url, (int)image_file_buf,
				(int)cap);
		if (rv < 0) {
			s_copy(err, "file not found", errcap);
			return -1;
		}
		*out = image_file_buf;
		*outlen = (unsigned int)rv;
		return 0;
	}
	rv = fetch_remote(url, cap, image_file_buf, cap, image_comp_buf,
			  BR_IMG_FILE_MAX, &fetch_meta, &tr, err, errcap);
	if (rv == FETCH_ABORT)
		return FETCH_ABORT;
	if (rv < 0)
		return -1;
	len = (unsigned int)rv;
	if (len == 0) {
		s_copy(err, "empty image", errcap);
		return -1;
	}
	*out = image_file_buf;
	*outlen = len;
	return 0;
}


/* Remote stylesheets are immutable enough for the current boot: keep
 * successful responses under /share so a reload, a second visit or a fresh
 * browser launch does not fetch the same five sheets over the network again.
 * /share is cleared by reboot/kexec; when the cache fills up it is reset. */
static int css_cache_count;

static unsigned int css_cache_hash(const char *s)
{
	unsigned int h = 2166136261u;

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static void css_cache_path(const char *url, char *out, unsigned int cap)
{
	unsigned int n = 0;

	s_append(out, &n, "/share/" CSS_CACHE_PREFIX, cap);
	s_append_dec(out, &n, css_cache_hash(url), cap);
}

static void css_cache_clear(void)
{
	struct sys_dirent d;
	char path[96];

	for (;;) {
		int i, found = 0;

		for (i = 0; u_syscall3(SYS_LIST_DIR, (int)"/share", i,
				       (int)&d) == 1; i++) {
			unsigned int n = 0;

			if (!starts_with(d.name, CSS_CACHE_PREFIX))
				continue;
			s_append(path, &n, "/share/", sizeof(path));
			s_append(path, &n, d.name, sizeof(path));
			if (u_syscall1(SYS_UNLINK, (int)path) < 0)
				return;
			found = 1;
			break;
		}
		if (!found)
			break;
	}
	css_cache_count = 0;
}

static void css_cache_init(void)
{
	struct sys_dirent d;
	int i, count = 0;

	for (i = 0; u_syscall3(SYS_LIST_DIR, (int)"/share", i,
			       (int)&d) == 1; i++)
		if (starts_with(d.name, CSS_CACHE_PREFIX))
			count++;
	if (count >= CSS_CACHE_MAX) {
		css_cache_clear();
		return;
	}
	css_cache_count = count;
}

static int css_cache_get(const char *url, unsigned char **out,
			 unsigned int *outlen)
{
	char path[96];
	int rn;

	css_cache_path(url, path, sizeof(path));
	rn = u_syscall3(SYS_READ_FILE, (int)path, (int)image_io_buf,
			(int)CSS_CACHE_FILE_MAX);
	if (rn <= 0 || (unsigned int)rn >= CSS_CACHE_FILE_MAX)
		return 0;
	*out = image_io_buf;
	*outlen = (unsigned int)rn;
	return 1;
}

static void css_cache_put(const char *url, const unsigned char *data,
			  unsigned int len)
{
	char path[96];

	if (css_cache_count >= CSS_CACHE_MAX || len == 0 ||
	    len >= CSS_CACHE_FILE_MAX)
		return;
	css_cache_path(url, path, sizeof(path));
	if (u_syscall3(SYS_WRITE_FILE, (int)path, (int)data,
		       (int)len) >= 0)
		css_cache_count++;
}

/* Read one stylesheet into the shared static buffer.  CSS is parsed
 * immediately, so it does not need the page heap; this leaves the heap for
 * the 2 MB page body, the rules and the scaled image bitmaps. */
static int fetch_css(const char *url, unsigned char **out, unsigned int *outlen,
		     char *err, unsigned int errcap)
{
	unsigned int len;
	int tr = 0;
	int rv;

	*out = 0;
	*outlen = 0;
	if (url[0] == '/') {
		rv = u_syscall3(SYS_READ_FILE, (int)url, (int)image_io_buf,
				(int)BR_CSS_MAX);
		if (rv < 0) {
			s_copy(err, "stylesheet not found", errcap);
			return -1;
		}
		*out = image_io_buf;
		*outlen = (unsigned int)rv;
		return 0;
	}
	if (css_cache_get(url, out, outlen))
		return 0;
	rv = fetch_remote(url, BR_CSS_MAX, image_io_buf, BR_CSS_MAX,
			  image_io_buf + BR_CSS_MAX, BR_CSS_COMP_MAX,
			  &fetch_meta, &tr, err, errcap);
	if (rv == FETCH_ABORT)
		return FETCH_ABORT;
	if (rv < 0)
		return -1;
	len = (unsigned int)rv;
	if (len == 0) {
		s_copy(err, "empty stylesheet", errcap);
		return -1;
	}
	*out = image_io_buf;
	*outlen = len;
	css_cache_put(url, image_io_buf, len);
	return 0;
}

static void redraw_now(void);

static char error_page[1024];

static void show_error(const char *title, const char *detail)
{
	char *page = error_page;
	unsigned int n = 0;

	s_append(page, &n, "<html><body><h2>", sizeof(error_page));
	s_append(page, &n, title, sizeof(error_page));
	s_append(page, &n, "</h2><p>", sizeof(error_page));
	{
		const char *p = detail;
		while (*p && n + 1 < sizeof(error_page)) {
			if (*p == '<') s_append(page, &n, "&lt;", sizeof(error_page));
			else if (*p == '>') s_append(page, &n, "&gt;", sizeof(error_page));
			else if (*p == '&') s_append(page, &n, "&amp;", sizeof(error_page));
			else page[n++] = *p;
			p++;
		}
	}
	page[n] = '\0';
	s_append(page, &n, "</p><p>", sizeof(error_page));
	s_append(page, &n, page_url, sizeof(error_page));
	s_append(page, &n, "</p><p><a href=\"/\">home</a></p></body></html>",
		 sizeof(error_page));
	br_parse(&doc, page, n);
	layout_valid = 0;
	preload_active = 0;
	preload_pending = 0;
	preload_wait = 0;
	s_copy(page_title, title, sizeof(page_title));
	page_loading = 0;
	set_status(title, 1, 0);
	ngui_invalidate(&ui);
}

static int decode_direct_image(const unsigned char *data, unsigned int len,
			       unsigned int node)
{
	if (image_count >= BR_MAX_IMAGES)
		return -1;
	if (decode_image_data(data, len, &images[0]) != 0)
		return -1;
	doc.nodes[node].img_index = 0;
	image_count = 1;
	return 0;
}

static int ancestor_is(unsigned int node, unsigned int tag)
{
	unsigned int p = doc.nodes[node].parent;

	while (p) {
		if (doc.nodes[p].tag == tag)
			return 1;
		p = doc.nodes[p].parent;
	}
	return 0;
}

static int load_stylesheets(void)
{
	static char seen[BR_MAX_SHEETS][BR_URL_MAX];
	unsigned int i, sheets = 0, nseen = 0;

	for (i = 0; i < doc.nnode && sheets < BR_MAX_SHEETS; i++) {
		struct br_node *nd = &doc.nodes[i];
		const char *rel, *href;
		char url[BR_URL_MAX];
		unsigned char *data;
		unsigned int len;
		char err[64];
		int rv;

		if (nd->type != BR_NODE_ELEM || nd->tag != BR_TAG_LINK)
			continue;
		rel = br_rel(&doc, i);
		if (!contains_ci(rel, "stylesheet"))
			continue;
		href = br_href(&doc, i);
		if (!href[0])
			continue;
		if (resolve_link(page_url, href, url, sizeof(url)) != 0)
			continue;
		{
			unsigned int k;
			int dup = 0;

			for (k = 0; k < nseen; k++)
				if (s_eq(seen[k], url)) {
					dup = 1;
					break;
				}
			if (dup)
				continue;
			if (nseen < BR_MAX_SHEETS) {
				s_copy(seen[nseen], url, BR_URL_MAX);
				nseen++;
			}
		}
		rv = fetch_css(url, &data, &len, err, sizeof(err));
		if (rv == FETCH_ABORT)
			return FETCH_ABORT;
		if (rv != 0)
			continue;
		if (len)
			br_parse_css_text(&doc, (const char *)data, len);
		sheets++;
	}
	br_parse_inline_css(&doc);
	br_reset_styles(&doc);
	layout_valid = 0;
	return 0;
}

static void preload_begin(void)
{
	preload_active = 1;
	preload_pending = 0;
	preload_wait = 1;
	preload_node = 0;
	preload_loaded = 0;
	preload_scroll = 0;
}

/* Styles are computed top-down during layout, but images inside a hidden
 * menu/carousel are never visited and keep img_top == 0.  Without this check
 * the preloader walks the DOM in node order and spends the whole image
 * budget on logos and off-canvas thumbnails the reader cannot see. */
static int node_hidden(unsigned int node)
{
	unsigned int path[32], n = 0, p = node, i;
	struct br_style st, parent;

	while (p && n < sizeof(path) / sizeof(path[0])) {
		path[n++] = p;
		p = doc.nodes[p].parent;
	}
	parent = (struct br_style){0};
	parent.font_px = 16;
	parent.display = BR_DISPLAY_BLOCK;
	for (i = n; i > 0; i--) {
		br_style_for(&doc, path[i - 1], &parent, &st);
		if (st.display == BR_DISPLAY_NONE || st.vis_hidden ||
		    st.opacity_hidden)
			return 1;
		parent = st;
	}
	return 0;
}

static struct br_style base_style(void);

static void style_for_node(unsigned int node, struct br_style *out)
{
	unsigned int p = doc.nodes[node].parent;
	struct br_style parent;

	if (p) {
		style_for_node(p, &parent);
		br_style_for(&doc, node, &parent, out);
	} else {
		parent = base_style();
		br_style_for(&doc, node, &parent, out);
	}
}

/* Load at most one image per call.  Images below the current viewport are
 * left pending until the page is scrolled; this keeps a long article with
 * many 4K screenshots from stalling on content the user cannot see yet. */
static void preload_step(void)
{
	unsigned int batch = 0;

	while (preload_active && preload_node < doc.nnode) {
		unsigned int i = preload_node;
		struct br_node *nd = &doc.nodes[i];
		const char *src;
		char url[BR_URL_MAX];
		unsigned char *data;
		unsigned int len;
		char err[64];
		char msg[64];
		unsigned int n = 0;
		int rv;

		if (nd->type != BR_NODE_ELEM) {
			preload_node++;
			continue;
		}
		if (nd->tag != BR_TAG_IMG) {
			if (nd->bg_img_index == -1) {
				struct br_style bs;

				style_for_node(i, &bs);
				if (bs.display != BR_DISPLAY_NONE &&
				    !bs.vis_hidden && bs.bg_image) {
					const char *bsrc =
						br_bg_image_url(bs.bg_image - 1);

					if (bsrc[0] &&
					    !contains_ci(bsrc, ".svg") &&
					    !starts_with(bsrc, "data:") &&
					    resolve_link(page_url, bsrc, url,
							 sizeof(url)) == 0) {
						if (image_count >=
						    BR_MAX_IMAGES) {
							preload_node =
								doc.nnode;
							break;
						}
						preload_node = i + 1;
						data = 0;
						len = 0;
						rv = fetch_image(url,
								 BR_IMG_FILE_MAX,
								 &data, &len,
								 err, sizeof(err));
						if (rv == FETCH_ABORT) {
							preload_active = 0;
							preload_pending = 0;
							return;
						}
						if (rv == 0 &&
						    decode_image_data(
							    data, len,
							    &images[image_count]) == 0) {
							nd->bg_img_index =
								(short)image_count;
							image_count++;
							preload_loaded++;
							layout_valid = 0;
						} else {
							nd->bg_img_index = -2;
						}
						ngui_invalidate(&ui);
						if (++batch >= 4)
							return;
						continue;
					}
				}
				nd->bg_img_index = -2;
			}
			preload_node++;
			continue;
		}
		if (nd->img_index != -1) {
			preload_node++;
			continue;
		}
		if (ancestor_is(i, BR_TAG_NOSCRIPT) || node_hidden(i)) {
			preload_node++;
			continue;
		}
		src = br_src(&doc, i);
		if (!src[0] || contains_ci(src, ".svg") ||
		    starts_with(src, "data:")) {
			preload_node++;
			continue;
		}
		if (resolve_link(page_url, src, url, sizeof(url)) != 0) {
			preload_node++;
			continue;
		}
		if (image_count >= BR_MAX_IMAGES) {
			preload_node = doc.nnode;
			break;
		}
		/* Do not stop at the first below-fold image: DOM order is
		 * not visual order, and a footer/ad image early in the DOM
		 * used to hide every later article image from the loader. */
		if (img_top[i] >
		    (unsigned int)(page_scroll.offset + page_rect.h + 160)) {
			preload_pending = 1;
			preload_node++;
			continue;
		}
		preload_node = i + 1;

		s_append(msg, &n, "Loading images ", sizeof(msg));
		s_append_dec(msg, &n, (unsigned int)preload_loaded + 1,
			     sizeof(msg));
		s_append(msg, &n, "...", sizeof(msg));
		set_status(msg, 0, 0);
		ngui_invalidate(&ui);

		data = 0;
		len = 0;
		rv = fetch_image(url, BR_IMG_FILE_MAX, &data, &len, err,
				 sizeof(err));
		if (rv == FETCH_ABORT) {
			preload_active = 0;
			preload_pending = 0;
			return;
		}
		if (rv == 0 &&
		    decode_image_data(data, len, &images[image_count]) == 0) {
			nd->img_index = (short)image_count;
			image_count++;
			preload_loaded++;
			layout_valid = 0;
		} else {
			/* Remember failures so a scroll does not retry an
			 * unreachable tracking/ad image with the full network
			 * timeout on every pass. */
			nd->img_index = -2;
		}
		ngui_invalidate(&ui);
		/* A full page layout after every image dominates loading on a
		 * long page.  Decode a small batch, then let the main loop draw
		 * once for the whole batch. */
		if (++batch >= 4)
			return;
	}

	preload_active = 0;
	if (preload_pending)
		preload_scroll = page_scroll.offset;
	else
		preload_pending = 0;
	set_status(post_status_text, post_status_error, post_status_ttl);
	ngui_invalidate(&ui);
}

static void history_push(const char *url)
{
	if (!url || !url[0])
		return;
	if (hist_len > 0 && hist_pos >= 0 && hist_pos < hist_len &&
	    s_eq(history[hist_pos], url))
		return;
	if (hist_len > 0 && hist_pos + 1 < hist_len)
		hist_len = hist_pos + 1;
	if (hist_len < BR_HISTORY) {
		s_copy(history[hist_len], url, BR_URL_MAX);
		hist_pos = hist_len;
		hist_len++;
	} else {
		int i;
		for (i = 1; i < BR_HISTORY; i++)
			s_copy(history[i - 1], history[i], BR_URL_MAX);
		s_copy(history[BR_HISTORY - 1], url, BR_URL_MAX);
		hist_pos = BR_HISTORY - 1;
	}
}

static void load_page(const char *url, int local, int push)
{
	unsigned char *body = 0;
	unsigned int len = 0;
	int truncated = 0;
	struct img_info image_info;
	char ctype[64];
	char final[BR_URL_MAX];
	char err[80];
	const char *detail = 0;
	int rv;

	final[0] = '\0';
	cancel_fetch = 0;
	preload_active = 0;
	preload_pending = 0;

	js_browser_stop();
	js_last_status[0] = '\0';
	js_last_error = 0;
	heap_clear();
	image_reset();
	layout_valid = 0;
	page_scroll.offset = 0;
	page_scroll.dragging = 0;
	br_parse_plain(&doc, "", 0);    /* blank while loading */
	page_title[0] = '\0';
	page_loading = 1;
	page_is_local = local;
	s_copy(page_url, url, sizeof(page_url));
	s_copy(url_in.text, page_url, NGUI_INPUT_MAX);
	url_in.len = s_len(url_in.text);
	url_in.cursor = url_in.len;
	url_in.scroll = 0;
	ngui_set_focus(&ui, 0);
	if (final[0])
		final[0] = '\0';
	if (push)
		history_push(url);
	set_status("Loading...", 0, 0);
	ngui_invalidate(&ui);
	/* heap_clear() detaches the fonts, so attach them again before the
	 * first frame or the URL bar flashes in the 8x16 bitmap fallback. */
	if (heap_start() != 0) {
		show_error("Out of memory", "Cannot allocate the page buffer");
		return;
	}
	ngui_font_attach(&ui);
	redraw_now();
	browser_rules_alloc();
	rv = read_url(url, BR_PAGE_MAX, &body, &len, &local, ctype,
		     sizeof(ctype), final, sizeof(final), err, sizeof(err),
		     &truncated);
	if (rv == FETCH_ABORT)
		return;
	if (rv != 0) {
		detail = err;
		show_error("Cannot load this page", detail);
		s_copy(url_in.text, page_url, NGUI_INPUT_MAX);
		url_in.len = s_len(url_in.text);
		url_in.cursor = url_in.len;
		url_in.scroll = 0;
		return;
	}
	if (!local)
		origin_cache_learn(url, final[0] ? final : url);
	page_is_local = local;
	if (final[0]) {
		s_copy(page_url, final, sizeof(page_url));
		s_copy(url_in.text, page_url, NGUI_INPUT_MAX);
		url_in.len = s_len(url_in.text);
		url_in.cursor = url_in.len;
		url_in.scroll = 0;
	}

	if (contains_ci(ctype, "text/plain")) {
		br_parse_plain(&doc, (const char *)body, len);

	} else if (contains_ci(ctype, "image/") ||
		   (!contains_ci(ctype, "text/html") &&
		    img_sniff(body, len, &image_info) == 0)) {
		unsigned int node = br_parse_image_doc(&doc, page_url);
		if (node)
			decode_direct_image(body, len, node);
		s_copy(page_title, "Image", sizeof(page_title));
	} else {
		br_set_viewport(ui.w);
		br_parse(&doc, (const char *)body, len);
		s_copy(page_title, doc.title[0] ? doc.title : page_url,
		       sizeof(page_title));
		if (load_stylesheets() == FETCH_ABORT)
			return;
		if (pending_action)
			return;
	}
	preload_begin();
	page_loading = 0;
	s_copy(url_in.text, page_url, NGUI_INPUT_MAX);
	url_in.len = s_len(url_in.text);
	url_in.cursor = url_in.len;
	url_in.scroll = 0;
	ngui_set_focus(&ui, 0);

	/* Paint the parsed page before fetching/running scripts.  A slow
	 * remote <script src> then does not hide the page behind the loading
	 * screen; the fetch helper pumps this UI while it waits. */
	set_status("Loading scripts...", 0, 0);
	redraw_now();
	run_page_scripts();
	if (pending_action)
		return;

	post_status_text[0] = '\0';
	post_status_error = 0;
	post_status_ttl = 0;
	if (js_last_status[0]) {
		s_copy(post_status_text, js_last_status,
		       sizeof(post_status_text));
		post_status_error = js_last_error;
		post_status_ttl = 8000;
	} else if (truncated) {
		s_copy(post_status_text, "Page truncated at the buffer limit",
		       sizeof(post_status_text));
		post_status_ttl = 6000;
	} else if (doc.truncated) {
		s_copy(post_status_text, "Page truncated by the parser",
		       sizeof(post_status_text));
		post_status_ttl = 6000;
	} else if (starts_with(page_url, "https://")) {
		s_copy(post_status_text, "HTTPS: certificate NOT verified",
		       sizeof(post_status_text));
		post_status_ttl = 6000;
	}
	set_status(post_status_text, post_status_error, post_status_ttl);
	ngui_invalidate(&ui);
}

static void load_home(int push)
{
	cancel_fetch = 0;
	preload_active = 0;
	preload_pending = 0;
	js_browser_stop();
	js_last_status[0] = '\0';
	js_last_error = 0;
	heap_clear();
	image_reset();
	layout_valid = 0;
	page_scroll.offset = 0;
	page_scroll.dragging = 0;
	br_parse_plain(&doc, "", 0);
	page_loading = 1;
	page_is_local = 0;
	s_copy(page_url, "about:home", sizeof(page_url));
	s_copy(url_in.text, page_url, NGUI_INPUT_MAX);
	url_in.len = s_len(url_in.text);
	url_in.cursor = url_in.len;
	url_in.scroll = 0;
	ngui_set_focus(&ui, 0);
	if (push)
		history_push(page_url);
	ngui_invalidate(&ui);
	/* See load_page(): reattach the fonts before the first redraw. */
	if (heap_start() != 0) {
		show_error("Out of memory", "Cannot allocate the browser heap");
		return;
	}
	ngui_font_attach(&ui);
	redraw_now();
	browser_rules_alloc();
	{
		static const char home[] =
			"<html><head><title>NebOS Browser</title><style>"
			"body { margin: 10px; color: #e4e6f0; }"
			"h1 { color: #7fb0ff; }"
			".box { background-color: #262b3a; margin: 12px; "
			"padding: 10px; }"
			"a { color: #7fb0ff; }"
			"pre { background-color: #32384a; }"
			"</style></head><body>"
			"<h1>NebOS Browser</h1>"
			"<p>A small HTML/CSS browser built on <b>ngui</b>, the "
			"shared HTTP client and the image library.</p>"
			"<div class=\"box\"><b>Try:</b>"
			"<ul><li><a href=\"/floppy1/DEMO.HTM\">/"
			"floppy1/DEMO.HTM</a> on the root floppy</li>"
			"<li><a href=\"/floppy1/JS.HTM\">/"
			"floppy1/JS.HTM</a> inline JavaScript demo</li>"
			"<li><a href=\"http://10.0.2.2:8000/\">"
			"http://10.0.2.2:8000/</a> on the QEMU host</li>"
			"<li><a href=\"https://example.com/\">"
			"https://example.com/</a> (TLS, not verified)</li>"
			"</ul></div>"
			"<p>Type a URL in the bar above.  Esc closes the "
			"window, Ctrl+L focuses the address bar.</p>"
			"<pre>javascript: inline + external scripts (limited DOM)\nforms: not supported"
			"\nimages: PNG JPEG GIF BMP TGA PNM ICO</pre>"
			"</body></html>";
		br_set_viewport(ui.w);
		br_parse(&doc, home, sizeof(home) - 1);
		br_parse_inline_css(&doc);
		br_reset_styles(&doc);
		s_copy(page_title, doc.title, sizeof(page_title));
	}
	post_status_text[0] = '\0';
	post_status_error = 0;
	post_status_ttl = 0;
	page_loading = 0;
	set_status("", 0, 0);
	s_copy(url_in.text, page_url, NGUI_INPUT_MAX);
	url_in.len = s_len(url_in.text);
	url_in.cursor = url_in.len;
	url_in.scroll = 0;
	ngui_set_focus(&ui, 0);
	ngui_invalidate(&ui);
}

/* Extensions the browser opens from the local filesystem.  A dotted input
 * with one of these still resolves relative to the cwd. */
static int local_file_ext(const char *s)
{
	static const char *const exts[] = {
		"htm", "html", "txt", "text", "md", "css", "js", "json", "xml",
		"png", "jpg", "jpeg", "gif", "bmp", "tga", "ppm", "pgm", "pbm",
		"ico", "cur", "pam", "svg", "elf", "bin", "ttf", "log", "ini",
		"cfg", "conf", 0
	};
	unsigned int i = 0, start = 0, len = 0, e;

	while (s[i] && s[i] != '/' && s[i] != ':' && s[i] != '?' &&
	       s[i] != '#') {
		if (s[i] == '.') {
			start = i + 1;
			len = 0;
		} else if (start) {
			len++;
		}
		i++;
	}
	if (!len)
		return 0;
	for (e = 0; exts[e]; e++) {
		const char *x = exts[e];
		unsigned int xn = s_len(x), k;

		if (xn != len)
			continue;
		for (k = 0; k < len; k++) {
			char a = s[start + k];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (a != x[k])
				break;
		}
		if (k == len)
			return 1;
	}
	return 0;
}

/* A dotted first component (cloudron.io) is a hostname, not a local file.
 * Skipping the VFS stat keeps a slow FAT/floppy lookup off the path from
 * Enter to the first network request. */
static int looks_like_host(const char *s)
{
	unsigned int i = 0;
	int dot = 0;

	while (s[i] && s[i] != '/' && s[i] != '?' && s[i] != '#' &&
	       s[i] != ':') {
		char c = s[i];

		if (c == '.') {
			dot = 1;
		} else if (!((c >= 'a' && c <= 'z') ||
			     (c >= 'A' && c <= 'Z') ||
			     (c >= '0' && c <= '9') ||
			     c == '-' || c == '_')) {
			return 0;
		}
		i++;
	}
	return dot && i > 0 && !local_file_ext(s);
}

static void load_remote(const char *url, int push)
{
	char canon[BR_URL_MAX];

	s_copy(canon, url, sizeof(canon));
	origin_cache_apply(canon, sizeof(canon));
	load_page(canon, 0, push);
}

static void navigate(const char *input, int push)
{
	char target[BR_URL_MAX];
	char *t;

	if (!input)
		return;
	s_copy(target, input, sizeof(target));
	t = trim(target);
	if (!t[0])
		return;
	if (s_eq(t, "about:home")) {
		load_home(push);
		return;
	}
	if (starts_with(t, "file://")) {
		make_abs_local(t + 7, target, sizeof(target));
		load_page(target, 1, push);
		return;
	}
	if (is_http(t)) {
		load_remote(t, push);
		return;
	}
	if (t[0] == '/') {
		make_abs_local(t, target, sizeof(target));
		load_page(target, 1, push);
		return;
	}
	if (!looks_like_host(t)) {
		struct sys_stat st;

		if (u_stat(t, &st) == 0 && !st.is_dir) {
			make_abs_local(t, target, sizeof(target));
			load_page(target, 1, push);
			return;
		}
	}
	{
		char with_scheme[BR_URL_MAX];
		unsigned int n = 0;
		s_append(with_scheme, &n, "http://", sizeof(with_scheme));
		s_append(with_scheme, &n, t, sizeof(with_scheme));
		load_remote(with_scheme, push);
	}
}

/* ---- layout ---- */

static struct br_style base_style(void)
{
	struct br_style s = { 0 };

	s.color = ui.theme.text;
	s.bg = ui.theme.bg;
	s.has_bg = 0;
	for (int i = 0; i < 4; i++) {
		s.border_c[i] = 0;
		s.border_w[i] = 0;
	}
	s.font_px = 16;
	s.bold = 0;
	s.italic = 0;
	s.mono = 0;
	s.underline = 0;
	s.align = BR_ALIGN_LEFT;
	s.display = BR_DISPLAY_BLOCK;
	s.no_bullet = 0;
	s.uppercase = 0;
	s.width_kind = s.max_width_kind = s.min_width_kind = 0;
	s.height_kind = s.min_height_kind = s.max_height_kind = 0;
	s.width = s.max_width = s.min_width = 0;
	s.height = s.min_height = s.max_height = 0;
	s.box_sizing = 0;
	s.ml_auto = s.mr_auto = 0;
	s.line_h_pct = 0;
	s.line_h_px = 0;
	s.mt = s.mb = s.ml = s.mr = 0;
	s.pt = s.pb = s.pl = s.pr = 0;
	return s;
}

static int css_size(unsigned char kind, int val, int base)
{
	if (kind == 1)
		return val;
	if (kind == 2)
		return base * val / 100;
	return -1;
}

static void draw_border_box(struct ngui *ui, struct ngui_rect r,
			    const struct br_style *st)
{
	int top = st->border_w[0], right = st->border_w[1];
	int bottom = st->border_w[2], left = st->border_w[3];

	if (r.w <= 0 || r.h <= 0)
		return;
	if (top >= r.h || bottom >= r.h || left >= r.w || right >= r.w) {
		int i;

		for (i = 0; i < 4; i++)
			if (st->border_w[i]) {
				ngui_fill_rect(ui, r, st->border_c[i]);
				break;
			}
		return;
	}
	if (top)
		ngui_fill_rect(ui, (struct ngui_rect){r.x, r.y, r.w, top},
			       st->border_c[0]);
	if (bottom)
		ngui_fill_rect(ui, (struct ngui_rect){r.x,
						      r.y + r.h - bottom,
						      r.w, bottom},
			       st->border_c[2]);
	if (left)
		ngui_fill_rect(ui, (struct ngui_rect){r.x, r.y + top, left,
						      r.h - top - bottom},
			       st->border_c[3]);
	if (right)
		ngui_fill_rect(ui, (struct ngui_rect){r.x + r.w - right,
						      r.y + top, right,
						      r.h - top - bottom},
			       st->border_c[1]);
}

static void draw_bg_image(struct lay *l, struct ngui_rect box,
			  unsigned int node, const struct br_style *st)
{
	struct br_image *bi;
	struct img_blit b;

	if (!st->bg_image || box.w <= 0 || box.h <= 0)
		return;
	if (doc.nodes[node].bg_img_index < 0 ||
	    doc.nodes[node].bg_img_index >= image_count)
		return;
	bi = &images[doc.nodes[node].bg_img_index];
	if (bi->state != 1 || bi->img.w <= 0)
		return;
	b.dst = l->ui->px;
	b.dst_w = l->ui->w;
	b.dst_h = l->ui->h;
	b.clip_x = page_rect.x;
	b.clip_y = page_rect.y;
	b.clip_w = page_rect.w - 12;
	b.clip_h = page_rect.h;
	b.x = box.x;
	b.y = box.y;
	b.scale = ((unsigned int)box.w << 16) / bi->img.w;
	b.bg = ui.theme.bg;
	b.bg2 = ui.theme.bg;
	b.flags = IMG_BLIT_OVER_DST;
	img_draw(&bi->img, &b);
}

static int run_width(const struct lay_run *r)
{
	if (r->img >= 0)
		return r->iw;
	if (r->img == -2)
		return r->iw;
	return (int)r->w;
}

static void flush_line(struct lay *l)
{
	int total, x, i, vis;

	if (!l->nruns && l->line_x == l->left) {
		l->line_h = 0;
		return;
	}
	total = l->line_x - l->left;
	x = l->left;
	vis = 1;
	if (l->draw) {
		int lh = l->line_h ? l->line_h : 16;

		vis = l->line_y + lh >= page_rect.y &&
		      l->line_y <= page_rect.y + page_rect.h;
	}
	if (l->align == BR_ALIGN_CENTER)
		x += (l->right - l->left - total) / 2;
	else if (l->align == BR_ALIGN_RIGHT)
		x = l->right - total;
	if (x < l->left)
		x = l->left;
	for (i = 0; i < l->nruns; i++) {
		struct lay_run *r = &l->runs[i];
		int w = run_width(r);
		int ry = l->line_y + r->dy;

		x += r->gap;
		if (l->draw && vis) {
			if (r->img == -2) {
				int hh = r->ih ? r->ih : 20;

				ngui_fill_rect(l->ui,
					(struct ngui_rect){x, ry, w, hh},
					l->ui->theme.panel_alt);
				ngui_frame(l->ui,
					(struct ngui_rect){x, ry, w, hh},
					l->ui->theme.border);
				ngui_text_px(l->ui, x + 6, ry + 2,
					     r->lit, s_len(r->lit), r->color,
					     r->font_px, r->bold, r->italic,
					     r->mono);
			} else if (r->img >= 0) {
				struct br_image *bi = &images[r->img];
				if (bi->state == 1) {
					struct img_blit b;
					b.dst = l->ui->px;
					b.dst_w = l->ui->w;
					b.dst_h = l->ui->h;
					b.clip_x = page_rect.x;
					b.clip_y = page_rect.y;
					b.clip_w = page_rect.w - 12;
					b.clip_h = page_rect.h;
					b.x = x;
					b.y = ry;
					b.scale = ((unsigned int)r->iw << 16) /
						  bi->img.w;
					b.bg = ui.theme.bg;
					b.bg2 = ui.theme.bg;
					b.flags = IMG_BLIT_OVER_DST;
					img_draw(&bi->img, &b);
				}
			} else {
				const char *s = r->has_lit ? r->lit :
						doc.text + r->off;
				unsigned int n = r->has_lit ? s_len(r->lit) :
						r->len;
				ngui_text_px(l->ui, x, ry, s, n,
					     r->color, r->font_px, r->bold,
					     r->italic, r->mono);
				if (r->underline)
					ngui_fill_rect(l->ui,
						(struct ngui_rect){x,
						 ry +
						 ngui_text_ascent(l->ui,
								  r->font_px,
								  r->mono) + 2,
						 w, 1}, r->color);
			}
			if (r->href) {
				if (nhits < HIT_MAX) {
					struct hit *h = &hits[nhits];
					int hh = r->img >= 0 ? r->ih :
						 ngui_text_line_height(l->ui,
						 r->font_px, r->mono);
					int merge = nhits > 0 &&
						hits[nhits - 1].node == r->href &&
						hits[nhits - 1].y == ry &&
						hits[nhits - 1].h == hh &&
						x <= hits[nhits - 1].x +
						     hits[nhits - 1].w + 2;
					if (merge) {
						struct hit *p = &hits[nhits - 1];
						int end = x + w;
						if (end > p->x + p->w)
							p->w = end - p->x;
					} else {
						h->x = x;
						h->y = ry;
						h->w = w;
						h->h = hh;
						h->node = r->href;
						nhits++;
					}
				}
			}
		}
		x += w;
	}
	l->y += l->line_h + 2;
	l->nruns = 0;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_h = 0;
	l->space_pending = 0;
}

static void add_run(struct lay *l, unsigned short off, unsigned short len,
		    const struct br_style *st, unsigned short href,
		    const char *lit)
{
	unsigned int n = lit ? s_len(lit) : len;
	const char *tp = lit ? lit : doc.text + off;
	int w = (int)ngui_text_width_px(l->ui, tp, n, st->font_px,
					st->bold, st->italic, st->mono);

	if (l->nruns >= (int)(sizeof(l->runs) / sizeof(l->runs[0])))
		flush_line(l);
	if (l->nruns == 0) {
		l->line_y = l->y;
		l->line_h = 0;
	}
	{
		struct lay_run *r = &l->runs[l->nruns++];
		int gap = l->line_x - l->run_x;

		if (gap < 0)
			gap = 0;
		r->gap = (unsigned short)gap;
		r->off = off;
		r->len = len;
		r->color = st->color;
		r->href = href;
		r->dy = 0;
		r->has_lit = 0;
		r->lit[0] = '\0';
		if (lit && lit[0]) {
			unsigned int i;
			for (i = 0; i + 1 < sizeof(r->lit) && lit[i]; i++)
				r->lit[i] = lit[i];
			r->lit[i] = '\0';
			r->has_lit = 1;
		}
		r->img = -1;
		r->iw = r->ih = 0;
		r->font_px = st->font_px;
		r->bold = st->bold;
		r->italic = st->italic;
		r->mono = st->mono;
		/* A control with its own background is a button, not body copy;
		 * an inherited link underline would strike through the fill. */
		r->underline = st->underline && !st->has_bg;
		r->w = (unsigned short)w;
		if (r->has_lit)
			r->w = (unsigned short)ngui_text_width_px(
				l->ui, r->lit, s_len(r->lit), r->font_px,
				r->bold, r->italic, r->mono);
		w = r->w;
	}
	l->line_x += w;
	l->run_x = l->line_x;
	{
		int lh = ngui_text_line_height(l->ui, st->font_px, st->mono);

		if (st->line_h_px)
			lh = st->line_h_px;
		else if (st->line_h_pct)
			lh = st->font_px * st->line_h_pct / 100;
		if (lh < ngui_text_line_height(l->ui, st->font_px, st->mono))
			lh = ngui_text_line_height(l->ui, st->font_px,
						   st->mono);
		if (l->line_h < lh)
			l->line_h = lh;
	}
}

static void add_image_run(struct lay *l, unsigned int node,
			  const struct br_style *st, unsigned short href)
{
	struct br_image *bi = 0;
	int idx = doc.nodes[node].img_index;
	int dw, dh, maxw;

	if (idx >= 0 && idx < image_count && images[idx].state == 1)
		bi = &images[idx];
	if (l->space_pending && (l->nruns > 0 || l->line_x > l->left)) {
		int sp = (int)ngui_text_width_px(l->ui, " ", 1, st->font_px,
						 st->bold, st->italic,
						 st->mono);
		if (l->line_x + sp <= l->right)
			l->line_x += sp;
	}
	l->space_pending = 0;
	if (!bi) {
		const char *alt = br_alt(&doc, node);
		struct br_style gs = *st;
		char tmp[96];
		unsigned int n = 0;
		int w;

		gs.color = BR_GRAY;
		gs.underline = 0;
		s_append(tmp, &n, "[", sizeof(tmp));
		s_append(tmp, &n, alt[0] ? alt : "image", sizeof(tmp));
		s_append(tmp, &n, "]", sizeof(tmp));
		w = (int)ngui_text_width_px(l->ui, tmp, n, gs.font_px,
					    gs.bold, gs.italic, gs.mono);
		if (l->line_x + w > l->right &&
		    (l->nruns > 0 || l->line_x > l->left))
			flush_line(l);
		add_run(l, 0, 0, &gs, href, tmp);
		if (!l->draw) {
			int t = l->line_y < 0 ? 0 : l->line_y;
			int hh = ngui_text_line_height(l->ui, gs.font_px,
						       gs.mono);

			if (t > 65535)
				t = 65535;
			if (hh > 65535)
				hh = 65535;
			img_top[node] = (unsigned short)t;
			img_h[node] = (unsigned short)hh;
		}
		return;
	}
	maxw = l->right - l->left;
	if (maxw < 8)
		maxw = 8;
	{
		int iw = bi->native_w, ih = bi->native_h;
		int have_w = 0, have_h = 0, css_w = 0, css_h = 0;

		if (iw <= 0 || ih <= 0) {
			iw = (int)bi->img.w;
			ih = (int)bi->img.h;
		}

		dw = iw;
		dh = ih;
		if (doc.nodes[node].width_attr) {
			int want = doc.nodes[node].width_attr;
			dw = doc.nodes[node].width_pct ? maxw * want / 100 : want;
			have_w = 1;
		}
		if (doc.nodes[node].height_attr) {
			int want = doc.nodes[node].height_attr;
			dh = doc.nodes[node].height_pct ? maxw * want / 100 : want;
			have_h = 1;
		}
		if (have_w && !have_h)
			dh = ih ? ih * dw / iw : 0;
		if (have_h && !have_w)
			dw = iw ? iw * dh / ih : 0;

		if (st->width_kind) {
			css_w = 1;
			dw = st->width_kind == 2 ? maxw * (int)st->width / 100 :
						   (int)st->width;
		}
		if (st->height_kind) {
			css_h = 1;
			dh = st->height_kind == 2 ? maxw * (int)st->height / 100 :
						    (int)st->height;
		}
		if (css_w && !css_h)
			dh = ih ? ih * dw / iw : 0;
		if (css_h && !css_w)
			dw = iw ? iw * dh / ih : 0;

		if (st->max_width_kind) {
			int mw = st->max_width_kind == 2 ?
				 maxw * (int)st->max_width / 100 :
				 (int)st->max_width;
			if (mw > 0 && dw > mw) {
				dh = iw ? ih * mw / iw : 0;
				dw = mw;
			}
		}
	}
	if (dw < 8)
		dw = 8;
	if (dh < 8)
		dh = 8;
	if (dw > maxw) {
		dh = dw ? dh * maxw / dw : dh;
		dw = maxw;
	}
	if (!l->draw) {
		int t = l->line_y < 0 ? 0 : l->line_y;

		if (t > 65535)
			t = 65535;
		if (dh > 65535)
			dh = 65535;
		img_top[node] = (unsigned short)t;
		img_h[node] = (unsigned short)dh;
	}
	if (l->line_x + dw > l->right && (l->nruns > 0 ||
					  l->line_x > l->left))
		flush_line(l);
	if (l->nruns >= (int)(sizeof(l->runs) / sizeof(l->runs[0])))
		flush_line(l);
	if (l->nruns == 0) {
		l->line_y = l->y;
		l->line_h = 0;
	}
	{
		struct lay_run *r = &l->runs[l->nruns++];
		int gap = l->line_x - l->run_x;

		if (gap < 0)
			gap = 0;
		r->gap = (unsigned short)gap;
		r->off = 0;
		r->len = 0;
		r->color = 0;
		r->href = href;
		/* Inline replaced elements honour their vertical margins; this
		 * is how Bootstrap's negative logo margins centre the wordmark. */
		r->dy = st->mt;
		r->has_lit = 0;
		r->lit[0] = '\0';
		r->img = (short)idx;
		r->iw = (unsigned short)dw;
		r->ih = (unsigned short)dh;
		r->w = (unsigned short)dw;
		r->font_px = st->font_px;
		r->bold = st->bold;
		r->italic = st->italic;
		r->mono = st->mono;
		r->underline = 0;
	}
	l->line_x += dw;
	l->run_x = l->line_x;
	if (l->line_h < dh)
		l->line_h = dh;
}

static int text_width(struct lay *l, const char *s, unsigned int n,
		      const struct br_style *st)
{
	return (int)ngui_text_width_px(l->ui, s, n, st->font_px,
				       st->bold, st->italic, st->mono);
}

/* Longest prefix that fits in maxw, at least one byte. */
static unsigned int fit_prefix(struct lay *l, const char *s, unsigned int n,
			       const struct br_style *st, int maxw)
{
	unsigned int take = 0;

	while (take < n) {
		if (text_width(l, s, take + 1, st) > maxw)
			break;
		take++;
	}
	return take ? take : 1;
}

static void emit_word(struct lay *l, const char *s, unsigned int start,
		      unsigned int n, unsigned int base,
		      const struct br_style *st, unsigned short href)
{
	int avail = l->right - l->left;
	int w = text_width(l, s + start, n, st);
	unsigned int off = start;

	if (st->nowrap) {
		if (n)
			add_run(l, (unsigned short)(base + start),
				(unsigned short)n, st, href, 0);
		return;
	}
	if (l->line_x + w > l->right &&
	    (l->nruns > 0 || l->line_x > l->left))
		flush_line(l);
	if (w <= avail) {
		if (n)
			add_run(l, (unsigned short)(base + start),
				(unsigned short)n, st, href, 0);
		return;
	}
	while (n > 0) {
		unsigned int take = fit_prefix(l, s + off, n, st, avail);

		add_run(l, (unsigned short)(base + off),
			(unsigned short)take, st, href, 0);
		off += take;
		n -= take;
		if (n)
			flush_line(l);
	}
}

static void emit_text(struct lay *l, const char *s, unsigned int len,
		      const struct br_style *st, unsigned short href, int pre)
{
	unsigned int i = 0;
	unsigned int base;
	int space_w = (int)ngui_text_width_px(l->ui, " ", 1, st->font_px,
					      st->bold, st->italic, st->mono);

	if (!s || !len)
		return;
	if (space_w <= 0)
		space_w = 8;
	base = (unsigned int)(s - doc.text);
	if (st->uppercase) {
		char *p = (char *)s;
		unsigned int j;

		for (j = 0; j < len; j++)
			if (p[j] >= 'a' && p[j] <= 'z')
				p[j] = (char)(p[j] - 'a' + 'A');
	}
	if (pre) {
		while (i < len) {
			unsigned int start;

			if (s[i] == '\n') {
				flush_line(l);
				i++;
				continue;
			}
			if (s[i] == ' ') {
				if (l->line_x + space_w > l->right &&
				    (l->nruns > 0 || l->line_x > l->left))
					flush_line(l);
				else
					l->line_x += space_w;
				i++;
				continue;
			}
			start = i;
			while (i < len && s[i] != ' ' && s[i] != '\n')
				i++;
			emit_word(l, s, start, i - start, base, st, href);
		}
		return;
	}
	while (i < len) {
		unsigned int start, n;

		if (s[i] == ' ') {
			l->space_pending = 1;
			i++;
			continue;
		}
		start = i;
		while (i < len && s[i] != ' ')
			i++;
		n = i - start;
		if (l->space_pending &&
		    (l->nruns > 0 || l->line_x > l->left)) {
			if (st->nowrap || l->line_x + space_w <= l->right)
				l->line_x += space_w;
		}
		l->space_pending = 0;
		emit_word(l, s, start, n, base, st, href);
	}
}

static int list_index(unsigned int parent, unsigned int node)
{
	unsigned int n = 0, i;

	for (i = doc.nodes[parent].first; i && i != node;
	     i = doc.nodes[i].next)
		if (doc.nodes[i].tag == BR_TAG_LI)
			n++;
	return (int)n + 1;
}

static const char *form_text(unsigned int node)
{
	const char *v = br_value(&doc, node);
	const char *p = br_placeholder(&doc, node);
	const char *t = br_type(&doc, node);

	if (s_eq(t, "hidden"))
		return 0;
	if (s_eq(t, "submit") || s_eq(t, "button") || s_eq(t, "reset"))
		return v[0] ? v : "Button";
	if (v[0])
		return v;
	if (p[0])
		return p;
	if (doc.nodes[node].tag == BR_TAG_TEXTAREA)
		return "Text";
	if (doc.nodes[node].tag == BR_TAG_SELECT)
		return "Select";
	return "Search";
}

static void add_form_run(struct lay *l, unsigned int node,
			 const struct br_style *st, unsigned short href)
{
	const char *text = form_text(node);
	struct lay_run *r;
	int tw, w, h, i;

	if (!text)
		return;
	tw = (int)ngui_text_width_px(l->ui, text, s_len(text), st->font_px,
				     st->bold, st->italic, st->mono);
	h = ngui_text_line_height(l->ui, st->font_px, st->mono) + 4;
	if (h < 20)
		h = 20;
	if (st->width_kind == 1)
		w = st->width;
	else if (st->width_kind == 2)
		w = (l->right - l->left) * (int)st->width / 100;
	else
		w = tw + 18;
	if (w < 64)
		w = 64;
	if (w > l->right - l->left)
		w = l->right - l->left;
	if (w < 8)
		w = 8;
	if (l->line_x + w > l->right &&
	    (l->nruns > 0 || l->line_x > l->left))
		flush_line(l);
	if (l->nruns >= 96)
		return;
	r = &l->runs[l->nruns++];
	r->gap = (unsigned short)(l->line_x - l->run_x);
	r->off = r->len = 0;
	r->color = st->color;
	r->href = href;
	r->dy = 0;
	r->has_lit = 1;
	for (i = 0; i + 1 < (int)sizeof(r->lit) && text[i]; i++)
		r->lit[i] = text[i];
	r->lit[i] = '\0';
	r->img = -2;
	r->iw = (unsigned short)w;
	r->ih = (unsigned short)h;
	r->w = (unsigned short)w;
	r->font_px = st->font_px;
	r->bold = st->bold;
	r->italic = st->italic;
	r->mono = st->mono;
	r->underline = 0;
	l->line_x += w;
	l->run_x = l->line_x;
	if (l->line_h < h)
		l->line_h = h;
}

/* A poster wrapper is absolutely positioned over the media slot; the
 * measured height of its picture is normally added back to the flow.  When a
 * loading-skeleton sibling already reserves that slot, adding both doubles
 * the media height and pushes the article text a full screen down. */
static int has_skeleton_sibling(unsigned int node)
{
	unsigned int p = doc.nodes[node].parent, c;

	for (c = doc.nodes[p].first; c; c = doc.nodes[c].next) {
		if (c == node || doc.nodes[c].type != BR_NODE_ELEM)
			continue;
		if (doc.nodes[c].class_off &&
		    contains_ci(doc.attrs + doc.nodes[c].class_off,
				"skeleton"))
			return 1;
	}
	return 0;
}

static void layout_children(struct lay *l, unsigned int parent,
			    const struct br_style *pst, int depth,
			    unsigned short link);
static void layout_flex_children(struct lay *l, unsigned int parent,
				 const struct br_style *pst, int depth,
				 unsigned short link);
static void layout_grid_children(struct lay *l, unsigned int parent,
				 const struct br_style *pst, int depth,
				 unsigned short link);
static void layout_grid_auto(struct lay *l, unsigned int parent,
			     const struct br_style *pst, int depth,
			     unsigned short link);
static int flex_item_width(struct lay *l, unsigned int n,
			   const struct br_style *cs, int avail);
static unsigned int layout_inline_blocks(struct lay *l, unsigned int first,
					 const struct br_style *pst, int depth,
					 unsigned short link);
static void layout_table_row(struct lay *l, unsigned int row,
			     const struct br_style *pst, int depth,
			     unsigned short link);

/* A flex container whose items are only inline spans is usually just a
 * heading/icon pair.  Our flex row positions such items without emitting
 * their inline text in some deep page contexts; normal inline flow renders
 * the same content correctly. */
static int flex_wants_row(unsigned int parent, const struct br_style *pst)
{
	unsigned int c;

	for (c = doc.nodes[parent].first; c; c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];
		struct br_style cs;

		if (nd->type == BR_NODE_TEXT)
			continue;
		br_style_for(&doc, c, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE)
			continue;
		if (cs.display != BR_DISPLAY_INLINE || cs.flex_grow ||
		    cs.width_kind || cs.has_bg ||
		    cs.border_w[0] || cs.border_w[1] ||
		    cs.border_w[2] || cs.border_w[3] ||
		    nd->tag == BR_TAG_IMG)
			return 1;
	}
	return 0;
}

static int css_radius(const struct br_style *st, int corner,
		      int w, int h)
{
	int r = st->radius[corner];

	if (r <= 0)
		return 0;
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	return r;
}

static void svg_viewbox(unsigned node, int *vw, int *vh)
{
	struct br_node *nd = &doc.nodes[node];

	if (nd->width_attr && nd->height_attr) {
		*vw = nd->width_attr;
		*vh = nd->height_attr;
	} else {
		*vw = 300;
		*vh = 150;
	}
	if (*vw < 1)
		*vw = 1;
	if (*vh < 1)
		*vh = 1;
}

static int svg_num(const char **pp)
{
	const char *p = *pp;
	int neg = 0, v = 0, frac = 0, scale = 1;

	while (*p == ' ' || *p == ',' || *p == '\t' ||
	       *p == '\n' || *p == '\r')
		p++;
	if (*p == '-') {
		neg = 1;
		p++;
	} else if (*p == '+') {
		p++;
	}
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		p++;
	}
	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9' && scale < 1000) {
			frac = frac * 10 + (*p - '0');
			scale *= 10;
			p++;
		}
	}
	v += frac / scale;
	*pp = p;
	return neg ? -v : v;
}

static void svg_path(struct ngui *ui, const char *d,
		     int ox, int oy, int dw, int dh, int vw, int vh,
		     unsigned short color)
{
	int cx = 0, cy = 0, sx = 0, sy = 0;
	char cmd = 0;
	const char *p = d;

	while (p && *p) {
		int rel, nx, ny;
		char c;

		while (*p == ' ' || *p == ',' || *p == '\t' ||
		       *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		if ((*p >= 'A' && *p <= 'Z') ||
		    (*p >= 'a' && *p <= 'z'))
			cmd = *p++;
		else if (!cmd) {
			p++;
			continue;
		}
		rel = cmd >= 'a' && cmd <= 'z';
		c = rel ? (char)(cmd - 'a' + 'A') : cmd;
		if (c == 'Z') {
			ngui_line(ui, ox + cx * dw / vw, oy + cy * dh / vh,
				  ox + sx * dw / vw, oy + sy * dh / vh,
				  color);
			cx = sx;
			cy = sy;
			cmd = 0;
			continue;
		}
		if (c == 'H' || c == 'V') {
			const char *save = p;
			int a = svg_num(&p);

			if (p == save) {
				p++;
				continue;
			}
			nx = cx;
			ny = cy;
			if (c == 'H')
				nx = rel ? cx + a : a;
			else
				ny = rel ? cy + a : a;
		} else {
			const char *save = p;
			int a = svg_num(&p);
			int b = svg_num(&p);

			if (p == save) {
				p++;
				continue;
			}
			if (c == 'M') {
				nx = rel ? cx + a : a;
				ny = rel ? cy + b : b;
				cx = sx = nx;
				cy = sy = ny;
				cmd = rel ? 'l' : 'L';
				continue;
			}
			if (c != 'L')
				break;
			nx = rel ? cx + a : a;
			ny = rel ? cy + b : b;
		}
		ngui_line(ui, ox + cx * dw / vw, oy + cy * dh / vh,
			  ox + nx * dw / vw, oy + ny * dh / vh, color);
		cx = nx;
		cy = ny;
	}
}

static void draw_svg(struct ngui *ui, unsigned node,
		     int ox, int oy, int dw, int dh, int vw, int vh,
		     const struct br_style *pst)
{
	unsigned int c;

	for (c = doc.nodes[node].first; c; c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];
		struct br_style st;
		const char *nm;

		if (nd->type != BR_NODE_ELEM || !nd->name_off)
			continue;
		nm = doc.attrs + nd->name_off;
		br_style_for(&doc, c, pst, &st);
		if (s_eq(nm, "rect")) {
			struct ngui_rect r;
			int rx = (int)nd->svg_rx * dw / vw;

			r.x = ox + (int)nd->svg_x * dw / vw;
			r.y = oy + (int)nd->svg_y * dh / vh;
			r.w = (int)nd->width_attr * dw / vw;
			r.h = (int)nd->height_attr * dh / vh;
			if (r.w < 1)
				r.w = 1;
			if (r.h < 1)
				r.h = 1;
			if (rx > r.w / 2)
				rx = r.w / 2;
			if (rx > r.h / 2)
				rx = r.h / 2;
			if (st.has_bg) {
				if (rx)
					ngui_fill_round_rect(ui, r, rx, rx,
							     rx, rx, st.bg);
				else
					ngui_fill_rect(ui, r, st.bg);
			}
			if (st.border_w[0]) {
				if (rx)
					ngui_frame_round(ui, r, rx, rx, rx, rx,
							 st.border_w[0],
							 st.border_c[0]);
				else
					draw_border_box(ui, r, &st);
			}
		} else if (s_eq(nm, "line")) {
			if (st.border_w[0])
				ngui_line(ui,
					  ox + (int)nd->svg_x * dw / vw,
					  oy + (int)nd->svg_y * dh / vh,
					  ox + (int)nd->width_attr * dw / vw,
					  oy + (int)nd->height_attr * dh / vh,
					  st.border_c[0]);
		} else if (s_eq(nm, "path")) {
			if (st.border_w[0] && nd->src_off)
				svg_path(ui, doc.attrs + nd->src_off,
					 ox, oy, dw, dh, vw, vh,
					 st.border_c[0]);
		} else if (s_eq(nm, "text")) {
			unsigned int t = nd->first;
			const char *s;
			unsigned int n, tw;
			int px = st.font_px ? st.font_px : 16;
			int tx, ty, anchor;

			if (!t || doc.nodes[t].type != BR_NODE_TEXT)
				continue;
			s = doc.text + doc.nodes[t].text_off;
			n = doc.nodes[t].text_len;
			tw = ngui_text_width_px(ui, s, n, px, st.bold,
						st.italic, st.mono);
			tx = ox + (int)nd->svg_x * dw / vw;
			ty = oy + (int)nd->svg_y * dh / vh -
			     ngui_text_ascent(ui, px, st.mono);
			anchor = st.align;
			if (nd->attr_flags & BR_ATTR_ANCHOR_MIDDLE)
				anchor = BR_ALIGN_CENTER;
			else if (nd->attr_flags & BR_ATTR_ANCHOR_END)
				anchor = BR_ALIGN_RIGHT;
			if (anchor == BR_ALIGN_CENTER)
				tx -= (int)tw / 2;
			else if (anchor == BR_ALIGN_RIGHT)
				tx -= (int)tw;
			ngui_text_px(ui, tx, ty, s, n, st.color, px,
				     st.bold, st.italic, st.mono);
		}
	}
}

static void layout_block(struct lay *l, unsigned int node,
			 const struct br_style *st, int depth,
			 unsigned short link)
{
	int old_left = l->left, old_right = l->right, old_align = l->align;
	int top, l0, r0;
	int flow_y, flow_left, flow_right, positioned, relative, sticky, shifted;
	int cbw, ml, mr, bl, br, pl, pr, bt, bb, pt, pb;
	int deco_w, deco_h, content_w, content_h, box_h;
	int definite_w = 0;
	int normal_top = 0;
	int rel_dx = 0, rel_dy = 0;

	if (depth > 24 || st->display == BR_DISPLAY_NONE)
		return;
	flow_y = l->y;
	flow_left = l->left;
	flow_right = l->right;
	positioned = st->pos == 1 || st->pos == 2;
	relative = st->pos == 3;
	sticky = st->pos == 4;
	shifted = relative || sticky || st->transform_set;
	if (positioned) {
		int def_w = st->width_kind == 1 ? st->width : -1;

		if (st->pos == 2) {
			int avail_w = page_rect.w - 12;

			if (st->right_set && def_w >= 0)
				l->left = page_rect.x + avail_w - st->right -
					  def_w;
			else
				l->left = page_rect.x +
					  (st->left_set ? st->left : 0);
			l->right = def_w >= 0 ? l->left + def_w :
						page_rect.x + avail_w;
			if (st->bottom_set && st->height_kind == 1)
				l->y = page_rect.y + page_rect.h -
				      st->bottom - st->height;
			else
				l->y = page_rect.y +
				      (st->top_set ? st->top : 0);
		} else {
			int left = flow_left + (st->left_set ? st->left : 0);
			unsigned int parent = doc.nodes[node].parent;

			if (st->right_set && def_w >= 0)
				left = flow_right - st->right - def_w;
			l->left = left;
			l->right = def_w >= 0 ? left + def_w :
						flow_right +
						(st->left_set ? st->left : 0);
			l->y = flow_y + (st->top_set ? st->top : 0);
			if (st->bottom_set && st->height_kind == 1 &&
			    parent < BR_MAX_NODES && block_h[parent] > 0)
				l->y = flow_y + block_h[parent] - st->bottom -
				      st->height;
		}
		old_left = l->left;
		old_right = l->right;
	} else if (relative) {
		rel_dx = st->left_set ? st->left :
			 (st->right_set ? -st->right : 0);
		rel_dy = st->top_set ? st->top :
			 (st->bottom_set ? -st->bottom : 0);
	}
	if (st->transform_set) {
		rel_dx += st->tx;
		rel_dy += st->ty;
	}
	if (doc.nodes[node].tag == BR_TAG_INPUT ||
	    doc.nodes[node].tag == BR_TAG_TEXTAREA ||
	    doc.nodes[node].tag == BR_TAG_SELECT) {
		if (!(doc.nodes[node].tag == BR_TAG_INPUT &&
		      s_eq(br_type(&doc, node), "hidden")))
			add_form_run(l, node, st, link);
		l->y += 20;
		return;
	}
	flush_line(l);
	l->y += st->mt;
	top = l->y;
	cbw = old_right - old_left;
	if (cbw < 8)
		cbw = 8;
	ml = st->ml;
	mr = st->mr;
	bl = st->border_w[3];
	br = st->border_w[1];
	pl = st->pl;
	pr = st->pr;
	bt = st->border_w[0];
	bb = st->border_w[2];
	pt = st->pt;
	pb = st->pb;
	deco_w = bl + br + pl + pr;
	deco_h = bt + bb + pt + pb;

	if (st->width_kind) {
		int w;

		if (st->width_kind == 3)
			w = cbw * st->width_calc_pct / 100 +
			    st->width_calc_px;
		else
			w = css_size(st->width_kind, st->width, cbw);
		if (w < 0)
			w = 0;
		content_w = st->box_sizing ? w - deco_w : w;
		definite_w = 1;
	} else {
		content_w = cbw - ml - mr - deco_w;
	}
	if (content_w < 0)
		content_w = 0;
	if (st->max_width_kind) {
		int mw = css_size(st->max_width_kind, st->max_width, cbw);
		int max_box = st->box_sizing ? mw : mw + deco_w;

		if (content_w + deco_w > max_box)
			content_w = max_box - deco_w;
	}
	if (st->min_width_kind) {
		int mw = css_size(st->min_width_kind, st->min_width, cbw);
		int min_box = st->box_sizing ? mw : mw + deco_w;

		if (content_w + deco_w < min_box) {
			content_w = min_box - deco_w;
			definite_w = 1;
		}
	}
	if (content_w < 8)
		content_w = 8;
	if (definite_w) {
		int free_w = cbw - ml - mr - deco_w - content_w;

		if (free_w < 0)
			free_w = 0;
		if (st->ml_auto && st->mr_auto) {
			ml = free_w / 2;
			mr = free_w - ml;
		} else if (st->ml_auto) {
			ml = free_w;
		} else if (st->mr_auto) {
			mr = free_w;
		}
	}
	l0 = old_left + ml;
	r0 = l0 + deco_w + content_w;
	if (r0 < l0 + 8)
		r0 = l0 + 8;
	normal_top = top;
	if (sticky) {
		int stick = page_rect.y + 4 + (st->top_set ? st->top : 0);

		if (normal_top < stick)
			rel_dy += stick - normal_top;
	}
	if (shifted && l->draw) {
		top += rel_dy;
		l0 += rel_dx;
		r0 += rel_dx;
	}
	if (l->draw && block_h[node] &&
	    top < page_rect.y + page_rect.h &&
	    top + block_h[node] > page_rect.y) {
		struct ngui_rect box = {l0, top, r0 - l0, block_h[node]};
		int rtl = css_radius(st, 0, box.w, box.h);
		int rtr = css_radius(st, 1, box.w, box.h);
		int rbr = css_radius(st, 2, box.w, box.h);
		int rbl = css_radius(st, 3, box.w, box.h);

		if (rtl || rtr || rbr || rbl) {
			if (st->has_bg) {
				int bw = st->border_w[0];

				if (bw > 0 && st->border_c[0] != st->bg &&
				    box.w > bw * 2 && box.h > bw * 2) {
					struct ngui_rect inner = {
						box.x + bw, box.y + bw,
						box.w - bw * 2, box.h - bw * 2
					};
					int itl = rtl > bw ? rtl - bw : 0;
					int itr = rtr > bw ? rtr - bw : 0;
					int ibr = rbr > bw ? rbr - bw : 0;
					int ibl = rbl > bw ? rbl - bw : 0;

					ngui_fill_round_rect(l->ui, box, rtl,
							     rtr, rbr, rbl,
							     st->border_c[0]);
					ngui_fill_round_rect(l->ui, inner,
							     itl, itr, ibr,
							     ibl, st->bg);
				} else {
					ngui_fill_round_rect(l->ui, box, rtl,
							     rtr, rbr, rbl,
							     st->bg);
				}
			} else if (st->border_w[0]) {
				ngui_frame_round(l->ui, box, rtl, rtr, rbr,
						 rbl, st->border_w[0],
						 st->border_c[0]);
			}
		} else {
			if (st->has_bg)
				ngui_fill_rect(l->ui, box, st->bg);
			if (st->bg_image)
				draw_bg_image(l, box, node, st);
			if (st->border_w[0] || st->border_w[1] ||
			    st->border_w[2] || st->border_w[3])
				draw_border_box(l->ui, box, st);
		}
	}
	l->left = l0 + bl + pl;
	l->right = l->left + content_w;
	if (l->right < l->left + 8)
		l->right = l->left + 8;
	l->align = st->align;
	l->y = top + bt + pt;

	if (doc.nodes[node].tag == BR_TAG_HR) {
		if (l->draw)
			ngui_fill_rect(l->ui, (struct ngui_rect){l->left,
				 l->y + 2, l->right - l->left, 1}, BR_GRAY);
		l->y += 8;
	} else if (doc.nodes[node].tag == BR_TAG_VIDEO) {
		int vw = l->right - l->left;
		int vh;

		if (doc.nodes[node].width_pct)
			vw = vw * doc.nodes[node].width_attr / 100;
		else if (doc.nodes[node].width_attr)
			vw = doc.nodes[node].width_attr;
		if (vw > l->right - l->left)
			vw = l->right - l->left;
		if (vw < 8)
			vw = 8;
		vh = vw * 9 / 16;
		if (vh > 240)
			vh = 240;
		if (vh < 80)
			vh = 80;
		if (l->draw) {
			ngui_fill_rect(l->ui, (struct ngui_rect){l->left, l->y,
							 vw, vh}, 0xD69A);
			ngui_frame(l->ui, (struct ngui_rect){l->left, l->y,
							     vw, vh}, 0x8410);
			ngui_text(l->ui, l->left + 8, l->y + 8, "[video]",
				  0x4208);
		}
		l->y += vh + 4;
	} else if (doc.nodes[node].tag == BR_TAG_SVG) {
		int vw, vh, dw = l->right - l->left;
		int dh;

		svg_viewbox(node, &vw, &vh);
		if (dw < 8)
			dw = 8;
		dh = dw * vh / vw;
		if (dh < 8)
			dh = 8;
		if (dh > 1400)
			dh = 1400;
		if (l->draw) {
			struct ngui_rect clip = {l->left, l->y, dw, dh};

			ngui_push_clip(l->ui, clip);
			draw_svg(l->ui, node, l->left, l->y, dw, dh,
				 vw, vh, st);
			ngui_pop_clip(l->ui);
		}
		l->y += dh;
	} else {
		if (doc.nodes[node].tag == BR_TAG_LI && st->list_item &&
		    !st->no_bullet) {
			char *m = li_marker[node];
			if (!m[0]) {
				unsigned int parent = doc.nodes[node].parent;
				if (parent &&
				    doc.nodes[parent].tag == BR_TAG_OL) {
					int idx = list_index(parent, node);
					unsigned int n = 0;
					s_append_dec(m, &n, (unsigned int)idx, 6);
					s_append(m, &n, ".", 6);
				} else {
					s_copy(m, "-", 6);
				}
			}
			if (l->draw) {
				int mw = (int)ngui_text_width_ui(l->ui, m);
				ngui_text(l->ui, l->left - mw - 6, l->y, m,
					  st->color);
			}
		}
		if (doc.nodes[node].tag == BR_TAG_TR)
			layout_table_row(l, node, st, depth + 1, link);
		else if (st->grid_auto)
			layout_grid_auto(l, node, st, depth + 1, link);
		else if (st->display == BR_DISPLAY_GRID && st->grid_ncols >= 2 &&
			 st->grid_gap > 0)
			layout_grid_children(l, node, st, depth + 1, link);
		else if (st->display == BR_DISPLAY_FLEX && !st->flex_col &&
			 flex_wants_row(node, st))
			layout_flex_children(l, node, st, depth + 1, link);
		else
			layout_children(l, node, st, depth + 1, link);
	}
	flush_line(l);
	if (st->height_kind == 1) {
		content_h = st->height;
		if (st->box_sizing)
			content_h -= deco_h;
	} else {
		content_h = l->y - (top + bt + pt);
	}
	if (content_h < 0)
		content_h = 0;
	if (st->max_height_kind) {
		int mh = css_size(st->max_height_kind, st->max_height, cbw);
		int max_box = st->box_sizing ? mh : mh + deco_h;

		if (content_h + deco_h > max_box)
			content_h = max_box - deco_h;
	}
	if (st->min_height_kind) {
		int mh = css_size(st->min_height_kind, st->min_height, cbw);
		int min_box = st->box_sizing ? mh : mh + deco_h;

		if (content_h + deco_h < min_box)
			content_h = min_box - deco_h;
	}
	if (content_h < 0)
		content_h = 0;
	box_h = deco_h + content_h;
	if (box_h < 0)
		box_h = 0;
	l->y = (shifted ? normal_top : top) + box_h;
	l->left = old_left;
	l->right = old_right;
	l->align = old_align;
	l->y += st->mb;
	if (!l->draw) {
		if (box_h > 65535)
			box_h = 65535;
		block_h[node] = (unsigned short)box_h;
	}
	if (positioned) {
		l->y = flow_y;
		l->left = flow_left;
		l->right = flow_right;
	}
}


static int text_node_blank(const struct br_node *nd)
{
	const char *t = doc.text + nd->text_off;
	unsigned int i;

	for (i = 0; i < nd->text_len; i++)
		if (t[i] != ' ' && t[i] != '\t' && t[i] != '\n' &&
		    t[i] != '\r')
			return 0;
	return 1;
}

/* Rough shrink-to-fit width for a float without an explicit width: sum the
 * text/image widths in the subtree, plus padding and margins.  This keeps
 * Bootstrap's floated nav items on one line instead of giving each the whole
 * row. */
static int text_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int intrinsic_width(struct ngui *ui, unsigned int node,
			   const struct br_style *pst, int depth)
{
	unsigned int c;
	int w = 0;

	if (depth > 12)
		return 0;
	for (c = doc.nodes[node].first; c; c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];
		struct br_style cs;

		if (nd->type == BR_NODE_TEXT) {
			const char *t = doc.text + nd->text_off;
			unsigned int tn = nd->text_len;

			if (doc.nodes[node].tag == BR_TAG_PRE) {
				w += (int)ngui_text_width_px(ui, t, tn,
					pst->font_px, pst->bold, pst->italic,
					pst->mono);
			} else {
				unsigned int ti = 0;
				int first = 1;
				int lead = w > 0 && tn > 0 && text_space(t[0]);
				int sp = (int)ngui_text_width_px(ui, " ", 1,
					pst->font_px, pst->bold, pst->italic,
					pst->mono);

				while (ti < tn) {
					unsigned int ts;

					while (ti < tn && text_space(t[ti]))
						ti++;
					ts = ti;
					while (ti < tn && !text_space(t[ti]))
						ti++;
					if (ti == ts)
						break;
					if (!first || lead)
						w += sp;
					lead = 0;
					w += (int)ngui_text_width_px(ui,
						t + ts, ti - ts,
						pst->font_px, pst->bold,
						pst->italic, pst->mono);
					first = 0;
				}
			}
			continue;
		}
		br_style_for(&doc, c, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE)
			continue;
		if (nd->tag == BR_TAG_BR) {
			w += 8;
			continue;
		}
		if (nd->tag == BR_TAG_IMG) {
			/* Measure the image as layout will draw it: a CSS or
			 * attribute height keeps the intrinsic aspect ratio, and
			 * a CSS width overrides the native pixel width.  Bootstrap
			 * sizes the Cloudron logo with height:40px only. */
			int idx = nd->img_index;
			int iw = 0, ih = 0;

			if (idx >= 0 && idx < image_count &&
			    images[idx].state == 1) {
				iw = (int)images[idx].native_w;
				ih = (int)images[idx].native_h;
				if (iw <= 0 || ih <= 0) {
					iw = (int)images[idx].img.w;
					ih = (int)images[idx].img.h;
				}
			}
			if (cs.width_kind == 1) {
				w += cs.width;
			} else if (cs.width_kind == 2) {
				w += 64;
			} else if (nd->width_attr) {
				w += nd->width_attr;
			} else if (nd->height_attr && iw && ih) {
				w += iw * (int)nd->height_attr / ih;
			} else if (cs.height_kind == 1 && iw && ih) {
				w += iw * (int)cs.height / ih;
			} else if (iw) {
				w += iw;
			} else {
				w += 64;
			}
			continue;
		}
		w += cs.pl + cs.pr + cs.ml + cs.mr;
		w += intrinsic_width(ui, c, &cs, depth + 1);
		if (w > 4096)
			break;
	}
	return w;
}

/* Lay consecutive floated block children side by side.  Bootstrap's grid is
 * the main user: .row > .col-md-* with float:left and a percentage width.
 * Each column is laid out with its own left/right edge and the row advances
 * to the tallest column.  Returns the first child not consumed. */
static unsigned int layout_float_group(struct lay *l, unsigned int first,
				       const struct br_style *pst, int depth,
				       unsigned short link)
{
	unsigned int n = first;
	int row_left = l->left, row_right = l->right;
	int row_top, row_bottom, xl, xr, any = 0;

	flush_line(l);
	row_top = l->y;
	row_bottom = l->y;
	xl = row_left;
	xr = row_right;
	while (n) {
		struct br_node *nd = &doc.nodes[n];
		struct br_style cs;
		unsigned short lk = link;
		int colw, right;

		if (nd->type == BR_NODE_TEXT) {
			if (text_node_blank(nd)) {
				n = nd->next;
				continue;
			}
			break;
		}
		br_style_for(&doc, n, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE) {
			n = nd->next;
			continue;
		}
		/* A float blockifies its box: Bootstrap floats inline
		 * anchors too (the navbar brand and its text links), and
		 * their padding must be honoured. */
		if (!cs.float_left)
			break;
		if (nd->tag == BR_TAG_A && nd->href_off)
			lk = (unsigned short)n;
		right = cs.float_left == 2;
		if (cs.width_kind == 2)
			colw = (row_right - row_left) * (int)cs.width / 100;
		else if (cs.width_kind == 1)
			colw = (int)cs.width;
		else {
			colw = intrinsic_width(l->ui, n, &cs, 0) +
			       cs.pl + cs.pr + cs.ml + cs.mr;
			if (colw < 8)
				colw = 8;
		}
		if (colw < 8)
			colw = 8;
		if (colw > row_right - row_left)
			colw = row_right - row_left;
		/* The float column width is already resolved. */
		cs.width_kind = 0;
		cs.max_width_kind = 0;
		if (right) {
			if (any && xr - colw < xl) {
				row_top = row_bottom;
				row_bottom = row_top;
				xl = row_left;
				xr = row_right;
			}
			if (xr - colw < xl)
				colw = xr - xl;
			if (colw < 8)
				colw = 8;
			l->left = xr - colw;
			if (l->left < row_left)
				l->left = row_left;
			l->right = xr;
			xr = l->left;
		} else {
			if (any && xl + colw > xr) {
				row_top = row_bottom;
				row_bottom = row_top;
				xl = row_left;
				xr = row_right;
			}
			l->left = xl;
			l->right = xl + colw;
			xl += colw;
		}
		if (l->right < l->left + 8)
			l->right = l->left + 8;
		l->y = row_top;
		l->line_x = l->left;
		l->run_x = l->left;
		l->line_y = l->y;
		l->line_h = 0;
		l->nruns = 0;
		l->space_pending = 0;
		l->align = cs.align;
		layout_block(l, n, &cs, depth + 1, lk);
		if (l->y > row_bottom)
			row_bottom = l->y;
		any = 1;
		n = nd->next;
	}
	l->left = row_left;
	l->right = row_right;
	l->y = row_bottom;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_y = l->y;
	l->line_h = 0;
	l->nruns = 0;
	l->space_pending = 0;
	return n;
}

static int flex_item_width(struct lay *l, unsigned int n,
			   const struct br_style *cs, int avail)
{
	struct br_node *nd = &doc.nodes[n];

	(void)l;
	int w;

	if (cs->width_kind == 2)
		w = avail * (int)cs->width / 100;
	else if (cs->width_kind == 1)
		w = (int)cs->width;
	else if (nd->tag == BR_TAG_IMG) {
		int idx = nd->img_index;

		if (idx >= 0 && idx < image_count &&
		    images[idx].state == 1)
			w = (int)images[idx].img.w;
		else if (nd->width_attr)
			w = (int)nd->width_attr;
		else
			w = 64;
	} else {
		w = intrinsic_width(l->ui, n, cs, 0);
		w += cs->pl + cs->pr + cs->ml + cs->mr;
	}
	if (w < 24)
		w = 24;
	if (w > avail)
		w = avail;
	return w;
}

/* A run of display:inline-block siblings (file tags, small buttons) belongs
 * on one line, not one full-width block each.  Place intrinsic widths from
 * left to right and wrap when the row is full. */
static unsigned int layout_inline_blocks(struct lay *l, unsigned int first,
					 const struct br_style *pst, int depth,
					 unsigned short link)
{
	unsigned int n = first;
	int row_left = l->left, row_right = l->right;
	int avail = row_right - row_left;
	int x = row_left, y = l->y, row_h = 0, any = 0;

	flush_line(l);
	y = l->y;
	while (n) {
		struct br_node *nd = &doc.nodes[n];
		struct br_style cs;
		unsigned short lk = link;
		int colw, h;

		if (nd->type == BR_NODE_TEXT) {
			if (!text_node_blank(nd))
				break;
			if (any)
				x += 4;
			n = nd->next;
			continue;
		}
		br_style_for(&doc, n, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE) {
			n = nd->next;
			continue;
		}
		if (cs.display != BR_DISPLAY_INLINE_BLOCK)
			break;
		if (nd->tag == BR_TAG_A && nd->href_off)
			lk = (unsigned short)n;
		colw = flex_item_width(l, n, &cs, avail);
		/* A few pixels of slack keep a single long token (boot.S)
		 * from wrapping at the exact content width. */
		colw += 8;
		if (colw > avail)
			colw = avail;
		cs.width_kind = 0;
		cs.max_width_kind = 0;
		if (any && x + colw > row_right) {
			y += row_h + 2;
			x = row_left;
			row_h = 0;
			any = 0;
		}
		if (any && x > row_left)
			x += 4;
		l->left = x;
		l->right = x + colw;
		if (l->right < l->left + 8)
			l->right = l->left + 8;
		l->y = y;
		l->line_x = l->left;
		l->run_x = l->left;
		l->line_y = l->y;
		l->line_h = 0;
		l->nruns = 0;
		l->space_pending = 0;
		l->align = cs.align;
		layout_block(l, n, &cs, depth + 1, lk);
		h = l->y - y;
		if (h > row_h)
			row_h = h;
		x += colw;
		any = 1;
		n = nd->next;
	}
	if (any)
		y += row_h;
	l->left = row_left;
	l->right = row_right;
	l->y = y;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_y = l->y;
	l->line_h = 0;
	l->nruns = 0;
	l->space_pending = 0;
	return n;
}

struct flex_item {
	unsigned int node;
	unsigned short link;
	int w;
};

static void layout_flex_children(struct lay *l, unsigned int parent,
				 const struct br_style *pst, int depth,
				 unsigned short link)
{
	struct flex_item it[32];
	unsigned int c;
	int nitem = 0, grow_count = 0, fixed = 0, grow_w, gap = pst->grid_gap;
	int row_left = l->left, row_right = l->right;
	int avail = row_right - row_left;
	int idx, row_top;

	if (avail < 160 || depth > 24) {
		layout_children(l, parent, pst, depth, link);
		return;
	}

	/* If direct text is not just whitespace, normal block flow handles it
	 * better than pretending it is a flex item. */
	for (c = doc.nodes[parent].first; c; c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];

		if (nd->type == BR_NODE_TEXT && !text_node_blank(nd)) {
			layout_children(l, parent, pst, depth, link);
			return;
		}
	}

	for (c = doc.nodes[parent].first;
	     c && nitem < (int)(sizeof(it) / sizeof(it[0]));
	     c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];
		struct br_style cs;

		if (nd->type == BR_NODE_TEXT)
			continue;
		br_style_for(&doc, c, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE)
			continue;
		it[nitem].node = c;
		it[nitem].link = (nd->tag == BR_TAG_A && nd->href_off) ?
				 (unsigned short)c : link;
		it[nitem].w = 0;
		if (cs.flex_grow && !cs.width_kind)
			grow_count++;
		else
			fixed += flex_item_width(l, c, &cs, avail);
		nitem++;
	}
	grow_w = grow_count ? (avail - fixed) / grow_count : 0;
	if (grow_w < 96)
		grow_w = 96;
	for (idx = 0; idx < nitem; idx++) {
		struct br_style cs;

		br_style_for(&doc, it[idx].node, pst, &cs);
		if (cs.flex_grow && !cs.width_kind)
			it[idx].w = grow_w;
		else
			it[idx].w = flex_item_width(l, it[idx].node, &cs, avail);
		if (it[idx].w > avail)
			it[idx].w = avail;
		if (it[idx].w < 8)
			it[idx].w = 8;
	}

	row_top = l->y;
	idx = 0;
	while (idx < nitem) {
		int start = idx, count = 0, row_w = 0;
		int free_w, x, extra = 0, row_h = 0, k;

		while (idx < nitem) {
			int add = it[idx].w + (count ? gap : 0);

			if (count && pst->flex_wrap != 2 &&
			    row_w + add > avail)
				break;
			row_w += add;
			count++;
			idx++;
		}
		if (!count) {
			count = 1;
			row_w = it[start].w;
			idx = start + 1;
		}
		free_w = avail - row_w;
		if (free_w < 0)
			free_w = 0;
		x = row_left;
		if (pst->justify == 1) {
			x += free_w / 2;
		} else if (pst->justify == 2) {
			x += free_w;
		} else if (pst->justify == 3) {
			extra = count > 1 ? free_w / (count - 1) : 0;
		} else if (pst->justify == 4) {
			extra = count ? free_w / count : 0;
			x += extra / 2;
		}
		for (k = 0; k < count; k++) {
			int node = (int)it[start + k].node;
			int h = block_h[node];

			if (h > row_h)
				row_h = h;
		}
		if (pst->height_kind == 1 && pst->height > row_h)
			row_h = pst->height;
		for (k = 0; k < count; k++) {
			struct br_style cs;
			int node = (int)it[start + k].node;
			int yoff = 0;

			br_style_for(&doc, node, pst, &cs);
			cs.width_kind = 0;
			cs.max_width_kind = 0;
			if (l->draw && pst->align_items &&
			    row_h > block_h[node]) {
				if (pst->align_items == 1)
					yoff = (row_h - block_h[node]) / 2;
				else if (pst->align_items == 2)
					yoff = row_h - block_h[node];
			}
			l->left = x;
			l->right = x + it[start + k].w;
			if (l->right < l->left + 8)
				l->right = l->left + 8;
			l->y = row_top + yoff;
			l->line_x = l->left;
			l->run_x = l->left;
			l->line_y = l->y;
			l->line_h = 0;
			l->nruns = 0;
			l->space_pending = 0;
			l->align = cs.align;
			layout_block(l, node, &cs, depth + 1, it[start + k].link);
			if (!l->draw) {
				int h = l->y - row_top;

				if (h > row_h)
					row_h = h;
			}
			x += it[start + k].w;
			if (k + 1 < count)
				x += gap + extra;
		}
		row_top += row_h;
	}
	l->left = row_left;
	l->right = row_right;
	l->y = row_top;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_y = l->y;
	l->line_h = 0;
	l->nruns = 0;
	l->space_pending = 0;
}

/* Two-column CSS grid: track sizes from grid-template-columns (px, %, fr)
 * plus the grid-area hints used by site shells (site notice/footer full
 * width, column-start left, page-content right).  Anything more elaborate
 * falls back to normal block flow. */
static int grid_track_width(unsigned char kind, unsigned short val,
			    int avail, int other)
{
	if (kind == 2)
		return avail * (int)val / 100;
	if (kind == 1)
		return val;
	return other;
}

static void layout_grid_children(struct lay *l, unsigned int parent,
				 const struct br_style *pst, int depth,
				 unsigned short link)
{
	unsigned int n;
	int row_left = l->left, row_right = l->right;
	int avail = row_right - row_left;
	int gap = pst->grid_gap;
	int track_total = avail - gap;
	int col0, col1, x0, x1;
	int row_y, row_bottom, have_left = 0, have_right = 0;
	unsigned int c;

	if (avail < 32 || depth > 24)
		return;
	if (track_total < 16)
		track_total = 16;

	col0 = grid_track_width((unsigned char)(pst->width_kind),
				pst->width, track_total, 0);
	col1 = grid_track_width((unsigned char)(pst->max_width_kind),
				pst->max_width, track_total, 0);
	if (pst->width_kind == 3 && pst->max_width_kind == 3) {
		int w0 = pst->width ? pst->width : 1;
		int w1 = pst->max_width ? pst->max_width : 1;

		col0 = track_total * w0 / (w0 + w1);
		col1 = track_total - col0;
	} else if (pst->width_kind == 3) {
		col0 = track_total - col1;
		if (col0 < 24)
			col0 = 24;
	} else if (pst->max_width_kind == 3) {
		col1 = track_total - col0;
		if (col1 < 24)
			col1 = 24;
	}
	if (col0 < 24)
		col0 = 24;
	if (col1 < 24)
		col1 = 24;
	if (col0 + col1 > track_total) {
		col1 = track_total - col0;
		if (col1 < 24)
			col1 = 24;
	}
	x0 = row_left;
	x1 = row_left + col0 + gap;

	/* Direct text makes a grid ambiguous; use block flow instead. */
	for (c = doc.nodes[parent].first; c; c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];

		if (nd->type == BR_NODE_TEXT && !text_node_blank(nd)) {
			layout_children(l, parent, pst, depth, link);
			return;
		}
	}

	row_y = row_bottom = l->y;
	n = doc.nodes[parent].first;
	while (n) {
		struct br_node *nd = &doc.nodes[n];
		struct br_style cs;
		unsigned short lk = link;
		int slot, x, w;

		if (nd->type == BR_NODE_TEXT) {
			n = nd->next;
			continue;
		}
		br_style_for(&doc, n, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE) {
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_A && nd->href_off)
			lk = (unsigned short)n;
		slot = cs.grid_slot;
		if (!slot) {
			if (!have_left)
				slot = 2;
			else if (!have_right)
				slot = 3;
			else
				slot = 2;
		}
		if (slot == 1) {
			if (have_left || have_right) {
				row_y = row_bottom;
				have_left = have_right = 0;
			}
			x = row_left;
			w = avail;
		} else if (slot == 2) {
			if (have_left) {
				row_y = row_bottom;
				have_left = have_right = 0;
			}
			x = x0;
			w = col0;
			have_left = 1;
		} else {
			if (have_right) {
				row_y = row_bottom;
				have_left = have_right = 0;
			}
			x = x1;
			w = col1;
			have_right = 1;
		}
		cs.width_kind = 0;
		cs.max_width_kind = 0;
		l->left = x;
		l->right = x + w;
		if (l->right < l->left + 8)
			l->right = l->left + 8;
		l->y = row_y;
		l->line_x = l->left;
		l->run_x = l->left;
		l->line_y = l->y;
		l->line_h = 0;
		l->nruns = 0;
		l->space_pending = 0;
		l->align = cs.align;
		layout_block(l, n, &cs, depth + 1, lk);
		if (l->y > row_bottom)
			row_bottom = l->y;
		n = nd->next;
	}
	l->left = row_left;
	l->right = row_right;
	l->y = row_bottom;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_y = l->y;
	l->line_h = 0;
	l->nruns = 0;
	l->space_pending = 0;
}

/* repeat(auto-fit, minmax(<min>, 1fr)): pick as many equal columns as fit,
 * capped at four.  Used by the project docs' card grids. */
static void layout_grid_auto(struct lay *l, unsigned int parent,
			     const struct br_style *pst, int depth,
			     unsigned short link)
{
	unsigned int n, c;
	int row_left = l->left, row_right = l->right;
	int avail = row_right - row_left;
	int gap = pst->grid_gap;
	int minw = pst->width_kind == 1 ? pst->width : 200;
	int ncols, colw, x, y = l->y, start_y = l->y, row_h = 0, col = 0;

	if (avail < 32 || depth > 24) {
		layout_children(l, parent, pst, depth, link);
		return;
	}
	for (c = doc.nodes[parent].first; c; c = doc.nodes[c].next) {
		struct br_node *nd = &doc.nodes[c];

		if (nd->type == BR_NODE_TEXT && !text_node_blank(nd)) {
			layout_children(l, parent, pst, depth, link);
			return;
		}
	}
	if (minw < 80)
		minw = 80;
	ncols = (avail + gap) / (minw + gap);
	if (ncols < 1)
		ncols = 1;
	if (ncols > 4)
		ncols = 4;
	colw = (avail - gap * (ncols - 1)) / ncols;
	if (colw < 24)
		colw = 24;
	x = row_left;
	n = doc.nodes[parent].first;
	while (n) {
		struct br_node *nd = &doc.nodes[n];
		struct br_style cs;
		unsigned short lk = link;
		int h;

		if (nd->type == BR_NODE_TEXT) {
			n = nd->next;
			continue;
		}
		br_style_for(&doc, n, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE) {
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_A && nd->href_off)
			lk = (unsigned short)n;
		cs.width_kind = 0;
		cs.max_width_kind = 0;
		l->left = x;
		l->right = x + colw;
		if (l->right < l->left + 8)
			l->right = l->left + 8;
		l->y = y;
		l->line_x = l->left;
		l->run_x = l->left;
		l->line_y = l->y;
		l->line_h = 0;
		l->nruns = 0;
		l->space_pending = 0;
		l->align = cs.align;
		layout_block(l, n, &cs, depth + 1, lk);
		h = l->y - y;
		if (h > row_h)
			row_h = h;
		col++;
		n = nd->next;
		if (col == ncols) {
			y += row_h + gap;
			x = row_left;
			row_h = 0;
			col = 0;
		} else {
			x += colw + gap;
		}
	}
	if (col)
		y += row_h;
	else if (y > start_y)
		y -= gap;
	l->left = row_left;
	l->right = row_right;
	l->y = y;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_y = l->y;
	l->line_h = 0;
	l->nruns = 0;
	l->space_pending = 0;
}

/* Column widths are the widest cell in each column across every row, so all
 * rows line up.  The table model is still deliberately basic: no colspan,
 * border-collapse or percentage columns. */
static void table_columns(struct lay *l, unsigned int table,
			  const struct br_style *pst, int avail,
			  int *raw, int maxcols, int *out_cols,
			  int *out_total)
{
	unsigned int r, c;
	int cols = 0, total = 0, fixed_sum = 0, free_raw = 0, free_cols = 0;
	int fixed[12];

	for (c = 0; c < (unsigned int)maxcols; c++) {
		raw[c] = 0;
		fixed[c] = 0;
	}
	/* Intrinsic widths of single-column cells. */
	for (r = doc.nodes[table].first; r; r = doc.nodes[r].next) {
		struct br_node *rn = &doc.nodes[r];
		int col = 0;

		if (rn->type != BR_NODE_ELEM || rn->tag != BR_TAG_TR)
			continue;
		for (c = rn->first; c; c = doc.nodes[c].next) {
			struct br_node *cn = &doc.nodes[c];
			struct br_style cs;
			int w, span;

			if (cn->type != BR_NODE_ELEM ||
			    (cn->tag != BR_TAG_TD && cn->tag != BR_TAG_TH))
				continue;
			span = cn->colspan ? cn->colspan : 1;
			br_style_for(&doc, c, pst, &cs);
			if (cs.display == BR_DISPLAY_NONE)
				continue;
			if (col + span > maxcols)
				span = maxcols - col;
			if (span < 1)
				break;
			w = intrinsic_width(l->ui, c, &cs, 0) + cs.pl + cs.pr +
			    cs.ml + cs.mr +
			    cs.border_w[0] + cs.border_w[1] +
			    cs.border_w[2] + cs.border_w[3];
			if (w < 24)
				w = 24;
			if (span == 1) {
				if (w > raw[col])
					raw[col] = w;
			}
			if (col + span > cols)
				cols = col + span;
			col += span;
		}
	}
	/* A colspan cell only has to be as wide as the sum of its columns. */
	for (r = doc.nodes[table].first; r; r = doc.nodes[r].next) {
		struct br_node *rn = &doc.nodes[r];
		int col = 0;

		if (rn->type != BR_NODE_ELEM || rn->tag != BR_TAG_TR)
			continue;
		for (c = rn->first; c; c = doc.nodes[c].next) {
			struct br_node *cn = &doc.nodes[c];
			struct br_style cs;
			int span, w, sum, i;

			if (cn->type != BR_NODE_ELEM ||
			    (cn->tag != BR_TAG_TD && cn->tag != BR_TAG_TH))
				continue;
			span = cn->colspan ? cn->colspan : 1;
			if (col + span > maxcols)
				span = maxcols - col;
			if (span < 1)
				break;
			if (span > 1) {
				br_style_for(&doc, c, pst, &cs);
				if (cs.display != BR_DISPLAY_NONE) {
					w = intrinsic_width(l->ui, c, &cs, 0) +
					    cs.pl + cs.pr + cs.ml + cs.mr +
					    cs.border_w[0] + cs.border_w[1] +
					    cs.border_w[2] + cs.border_w[3];
					sum = 0;
					for (i = 0; i < span; i++)
						sum += raw[col + i];
					if (w > sum)
						raw[col + span - 1] += w - sum;
				}
			}
			col += span;
		}
	}
	/* Explicit and percentage widths override intrinsic widths. */
	for (r = doc.nodes[table].first; r; r = doc.nodes[r].next) {
		struct br_node *rn = &doc.nodes[r];
		int col = 0;

		if (rn->type != BR_NODE_ELEM || rn->tag != BR_TAG_TR)
			continue;
		for (c = rn->first; c; c = doc.nodes[c].next) {
			struct br_node *cn = &doc.nodes[c];
			struct br_style cs;
			int span;

			if (cn->type != BR_NODE_ELEM ||
			    (cn->tag != BR_TAG_TD && cn->tag != BR_TAG_TH))
				continue;
			span = cn->colspan ? cn->colspan : 1;
			if (col + span > maxcols)
				span = maxcols - col;
			if (span < 1)
				break;
			if (span == 1) {
				br_style_for(&doc, c, pst, &cs);
				if (cs.display != BR_DISPLAY_NONE) {
					if (cs.width_kind == 1) {
						raw[col] = cs.width;
						fixed[col] = 1;
					} else if (cs.width_kind == 2) {
						raw[col] = avail * cs.width / 100;
						fixed[col] = 1;
					}
				}
			}
			col += span;
		}
	}
	for (c = 0; c < (unsigned int)cols; c++) {
		if (raw[c] < 8)
			raw[c] = 8;
		total += raw[c];
		if (fixed[c])
			fixed_sum += raw[c];
		else {
			free_raw += raw[c];
			free_cols++;
		}
	}
	if (fixed_sum > 0 && fixed_sum < avail) {
		int free_avail = avail - fixed_sum;
		int i;

		if (free_raw > 0) {
			for (i = 0; i < cols; i++)
				if (!fixed[i])
					raw[i] = free_avail * raw[i] /
						 free_raw;
		} else if (free_cols > 0) {
			for (i = 0; i < cols; i++)
				if (!fixed[i])
					raw[i] = free_avail / free_cols;
		}
		total = 0;
		for (i = 0; i < cols; i++)
			total += raw[i];
	} else if (total < avail && total > 0) {
		int i;

		for (i = 0; i < cols; i++)
			raw[i] = raw[i] * avail / total;
		total = 0;
		for (i = 0; i < cols; i++)
			total += raw[i];
	}
	*out_cols = cols;
	*out_total = total;
}

static void layout_table_row(struct lay *l, unsigned int row,
			     const struct br_style *pst, int depth,
			     unsigned short link)
{
	unsigned int n, cell[12];
	int raw[12], colstart[12], colspan[12];
	int prefix[13];
	int ncell = 0, total = 0, cols = 0, col = 0;
	int row_left = l->left, row_right = l->right;
	int avail = row_right - row_left;
	int y = l->y, bottom = l->y;

	for (n = doc.nodes[row].first; n; n = doc.nodes[n].next) {
		struct br_node *nd = &doc.nodes[n];
		struct br_style cs;
		int span;

		if (nd->type == BR_NODE_TEXT) {
			if (text_node_blank(nd))
				continue;
			layout_children(l, row, pst, depth, link);
			return;
		}
		if (nd->tag != BR_TAG_TD && nd->tag != BR_TAG_TH) {
			layout_children(l, row, pst, depth, link);
			return;
		}
		br_style_for(&doc, n, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE)
			continue;
		if (ncell >= 12) {
			layout_children(l, row, pst, depth, link);
			return;
		}
		span = nd->colspan ? nd->colspan : 1;
		if (span > 12 - col)
			span = 12 - col;
		if (span < 1)
			continue;
		cell[ncell] = n;
		colstart[ncell] = col;
		colspan[ncell] = span;
		col += span;
		ncell++;
	}
	if (ncell < 1 || avail < 32) {
		layout_children(l, row, pst, depth, link);
		return;
	}
	table_columns(l, doc.nodes[row].parent, pst, avail, raw, 12, &cols,
		      &total);
	if (cols < 2 || total <= 0) {
		int c;

		for (c = 0; c < ncell; c++) {
			struct br_style cs;

			br_style_for(&doc, cell[c], pst, &cs);
			raw[c] = intrinsic_width(l->ui, cell[c], &cs, 0) +
				 cs.pl + cs.pr + cs.ml + cs.mr;
			if (raw[c] < 24)
				raw[c] = 24;
			total += raw[c];
		}
	}
	prefix[0] = row_left;
	for (n = 0; n < (unsigned int)cols && n < 12; n++)
		prefix[n + 1] = prefix[n] + raw[n];
	for (n = 0; n < (unsigned int)ncell; n++) {
		struct br_style cs;
		int start = colstart[n];
		int end = start + colspan[n];
		int w;

		if (end > cols)
			end = cols;
		if (start >= cols)
			continue;
		w = prefix[end] - prefix[start];
		br_style_for(&doc, cell[n], pst, &cs);
		/* The column width already reflects the cell's width property. */
		cs.width_kind = 0;
		cs.max_width_kind = 0;
		if (w < 24)
			w = 24;
		if (w < 8)
			w = 8;
		l->left = prefix[start];
		l->right = l->left + w;
		l->y = y;
		l->line_x = l->left;
		l->run_x = l->left;
		l->line_y = l->y;
		l->line_h = 0;
		l->nruns = 0;
		l->space_pending = 0;
		l->align = cs.align;
		layout_block(l, cell[n], &cs, depth + 1, link);
		if (l->y > bottom)
			bottom = l->y;
	}
	l->left = row_left;
	l->right = row_right;
	l->y = bottom;
	l->line_x = l->left;
	l->run_x = l->left;
	l->line_y = l->y;
	l->line_h = 0;
	l->nruns = 0;
	l->space_pending = 0;
}

/* Positioned elements (fixed headers, overlays) are drawn after the normal
 * flow so they sit above it, like CSS painting order.  Their flow origin is
 * saved for absolute boxes; fixed boxes only need the viewport rect. */
struct br_deferred {
	unsigned int node;
	int flow_y, flow_left, flow_right;
	unsigned short link;
	signed short z_index;
	unsigned char z_set;
	unsigned int order;
};
static struct br_deferred deferred[16];
static int ndeferred;

static void layout_children(struct lay *l, unsigned int parent,
			    const struct br_style *pst, int depth,
			    unsigned short link)
{
	unsigned int n;

	if (depth > 24)
		return;
	n = doc.nodes[parent].first;
	while (n) {
		struct br_node *nd = &doc.nodes[n];
		struct br_style cs;
		unsigned short lk = link;

		if (nd->type == BR_NODE_TEXT) {
			emit_text(l, doc.text + nd->text_off, nd->text_len, pst,
				  link, (nd->flags & BR_NODE_PREFORMAT) != 0);
			n = nd->next;
			continue;
		}
		br_style_for(&doc, n, pst, &cs);
		if (cs.display == BR_DISPLAY_NONE) {
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_A && nd->href_off)
			lk = (unsigned short)n;
		if (cs.pos == 1 || cs.pos == 2) {
			if (l->draw && ndeferred < 16) {
				deferred[ndeferred].node = n;
				deferred[ndeferred].flow_y = l->y;
				deferred[ndeferred].flow_left = l->left;
				deferred[ndeferred].flow_right = l->right;
				deferred[ndeferred].link = lk;
				deferred[ndeferred].z_index = cs.z_index;
				deferred[ndeferred].z_set = cs.z_set;
				deferred[ndeferred].order = (unsigned int)ndeferred;
				ndeferred++;
			} else {
				layout_block(l, n, &cs, depth + 1, lk);
			}
			if (cs.pos == 1 && block_h[n] &&
			    doc.nodes[n].class_off) {
				const char *cl = doc.attrs +
					doc.nodes[n].class_off;

				if ((contains_ci(cl, "poster-wrapper") ||
				     contains_ci(cl, "ts-picture")) &&
				    !has_skeleton_sibling(n))
					l->y += block_h[n];
			}
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_BR) {
			flush_line(l);
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_HR) {
			flush_line(l);
			if (l->draw)
				ngui_fill_rect(l->ui,
					(struct ngui_rect){l->left, l->y + 2,
					l->right - l->left, 1}, BR_GRAY);
			l->y += 8;
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_INPUT || nd->tag == BR_TAG_TEXTAREA ||
		    nd->tag == BR_TAG_SELECT) {
			if (!(nd->tag == BR_TAG_INPUT &&
			      s_eq(br_type(&doc, n), "hidden")))
				add_form_run(l, n, &cs, lk);
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_IMG) {
			add_image_run(l, n, &cs, lk);
			n = nd->next;
			continue;
		}
		if (nd->tag == BR_TAG_VIDEO) {
			layout_block(l, n, &cs, depth + 1, lk);
			n = nd->next;
			continue;
		}
		if (cs.float_left) {
			n = layout_float_group(l, n, pst, depth, lk);
			continue;
		}
		if (cs.display == BR_DISPLAY_INLINE_BLOCK) {
			n = layout_inline_blocks(l, n, pst, depth, link);
			continue;
		}
		if (cs.display == BR_DISPLAY_BLOCK || br_tag_is_block(nd->tag))
			layout_block(l, n, &cs, depth + 1, lk);
		else {
			layout_children(l, n, &cs, depth + 1, lk);
			if (nd->tag == BR_TAG_TD || nd->tag == BR_TAG_TH)
				l->space_pending = 1;
		}
		n = nd->next;
	}
}

/* One layout state, kept out of the 8 KB user stack: it holds 96 line
 * runs and a recursive page can otherwise overflow the stack. */
static struct lay layout_state;

static int measure_page(int left, int right)
{
	struct lay *l = &layout_state;
	struct br_style base = base_style();
	unsigned int i;

	for (i = 0; i < BR_MAX_NODES; i++) {
		block_h[i] = 0;
		img_top[i] = 0;
		img_h[i] = 0;
	}
	l->draw = 0;
	l->ui = 0;
	l->left = left;
	l->right = right;
	l->y = 0;
	l->line_x = left;
	l->run_x = left;
	l->line_y = 0;
	l->line_h = 0;
	l->align = BR_ALIGN_LEFT;
	l->depth = 0;
	l->nruns = 0;
	l->space_pending = 0;
	layout_children(l, doc.root, &base, 0, 0);
	return l->y + 8;
}

static int deferred_cmp(const struct br_deferred *a,
			const struct br_deferred *b)
{
	int az = a->z_set ? a->z_index : 0;
	int bz = b->z_set ? b->z_index : 0;

	if (az != bz)
		return az < bz ? -1 : 1;
	if (a->order != b->order)
		return a->order < b->order ? -1 : 1;
	return 0;
}

static void sort_deferred(void)
{
	int i;

	for (i = 1; i < ndeferred; i++) {
		struct br_deferred key = deferred[i];
		int j = i;

		while (j > 0 && deferred_cmp(&deferred[j - 1], &key) > 0) {
			deferred[j] = deferred[j - 1];
			j--;
		}
		deferred[j] = key;
	}
}

static void render_page(int left, int right, int y0)
{
	struct lay *l = &layout_state;
	struct br_style base = base_style();
	l->draw = 1;
	l->ui = &ui;
	l->left = left;
	l->right = right;
	l->y = y0;
	l->line_x = left;
	l->run_x = left;
	l->line_y = y0;
	l->line_h = 0;
	l->align = BR_ALIGN_LEFT;
	l->depth = 0;
	l->nruns = 0;
	l->space_pending = 0;
	nhits = 0;
	ndeferred = 0;
	layout_children(l, doc.root, &base, 0, 0);
	sort_deferred();
	{
		int i;

		for (i = 0; i < ndeferred; i++) {
			struct br_style st;

			br_style_for(&doc, deferred[i].node, &base, &st);
			if (st.display == BR_DISPLAY_NONE)
				continue;
			l->draw = 1;
			l->left = deferred[i].flow_left;
			l->right = deferred[i].flow_right;
			l->y = deferred[i].flow_y;
			l->line_x = l->left;
			l->run_x = l->left;
			l->line_y = l->y;
			l->line_h = 0;
			l->nruns = 0;
			l->space_pending = 0;
			l->align = st.align;
			if (st.pos == 2 && block_h[deferred[i].node]) {
				struct ngui_rect clip;
				int ch = block_h[deferred[i].node] + 24;

				clip.x = page_rect.x;
				clip.y = page_rect.y;
				clip.w = page_rect.w - 12;
				if (ch > page_rect.h)
					ch = page_rect.h;
				clip.h = ch;
				ngui_push_clip(&ui, clip);
				if (st.has_bg)
					ngui_fill_rect(&ui, clip, st.bg);
				layout_block(l, deferred[i].node, &st, 0,
					     deferred[i].link);
				ngui_pop_clip(&ui);
			} else {
				layout_block(l, deferred[i].node, &st, 0,
					     deferred[i].link);
			}
		}
	}
}

/* ---- UI ---- */

static int find_hit(int x, int y)
{
	int i;

	for (i = nhits - 1; i >= 0; i--) {
		struct hit *h = &hits[i];
		if (x >= h->x && y >= h->y && x < h->x + h->w &&
		    y < h->y + h->h)
			return (int)h->node;
	}
	return 0;
}

static void draw_toolbar(struct ngui_rect r, unsigned int *action)
{
	int x = r.x + 4, y = r.y + 4, h = 22;
	int compact = r.w < 560;
	int go_w = r.w < 420 ? 32 : 42;
	int in_x, in_w, res;

	ngui_fill_rect(&ui, r, ui.theme.panel);
	ngui_fill_rect(&ui, (struct ngui_rect){r.x, r.y + r.h - 1, r.w, 1},
		       ui.theme.border);

	if (ngui_icon_button(&ui, ID_BACK, (struct ngui_rect){x, y, 28, h},
			     NGUI_ICON_CHEVRON_LEFT))
		*action |= ACT_BACK;
	x += 32;
	if (!compact) {
		if (ngui_icon_button(&ui, ID_FORWARD,
				     (struct ngui_rect){x, y, 28, h},
				     NGUI_ICON_CHEVRON_RIGHT))
			*action |= ACT_FORWARD;
		x += 32;
	}
	if (ngui_icon_button(&ui, ID_RELOAD, (struct ngui_rect){x, y, 28, h},
			     NGUI_ICON_REFRESH))
		*action |= ACT_RELOAD;
	x += 32;
	if (!compact) {
		if (ngui_icon_button(&ui, ID_HOME,
				     (struct ngui_rect){x, y, 28, h},
				     NGUI_ICON_HOME))
			*action |= ACT_HOME;
		x += 32;
	}

	in_x = x + 2;
	in_w = r.x + r.w - 6 - go_w - 4 - in_x;
	if (in_w < 60)
		in_w = 60;
	res = ngui_text_input(&ui, ID_URL,
			      (struct ngui_rect){in_x, y, in_w, h}, &url_in);
	if (res & NGUI_INPUT_SUBMIT)
		*action |= ACT_NAV;
	if (res & NGUI_INPUT_CANCEL)
		ngui_set_focus(&ui, 0);
	if (ngui_button(&ui, ID_GO,
			(struct ngui_rect){r.x + r.w - 4 - go_w, y, go_w, h},
			"Go"))
		*action |= ACT_NAV;
}

static int page_hover_node(void)
{
	if (!ui.inside)
		return 0;
	if (ui.mouse_x < page_rect.x ||
	    ui.mouse_x >= page_rect.x + page_rect.w - 12 ||
	    ui.mouse_y < page_rect.y ||
	    ui.mouse_y >= page_rect.y + page_rect.h)
		return 0;
	return find_hit(ui.mouse_x, ui.mouse_y);
}

static void blit_page(struct ngui_rect page, unsigned short *src, int sw)
{
	int x, y, w = page.w, h = page.h;

	if (!src || w <= 0 || h <= 0)
		return;
	for (y = 0; y < h; y++) {
		unsigned short *s = src + (page.y + y) * sw + page.x;
		unsigned short *d = ui.px + (page.y + y) * ui.w + page.x;

		for (x = 0; x + 1 < w; x += 2) {
			*(unsigned int *)(d + x) = *(unsigned int *)(s + x);
		}
		if (x < w)
			d[x] = s[x];
	}
}

static void draw_page(struct ngui_rect page)
{
	int left, right, ch;
	int old_panel, old_dim;
	unsigned short *saved_px;
	int saved_w, saved_h, use_buf;

	page_rect = page;
	if (page_backbuf && pagebuf_w == ui.w && pagebuf_h == ui.h &&
	    page_cache_valid && layout_valid &&
	    !ui.pressed && !(ui.buttons & 1) && !ui.released &&
	    page_scroll.offset == page_cache_scroll &&
	    page_cache_nnode == doc.nnode &&
	    page_cache_x == page.x && page_cache_y == page.y &&
	    page_cache_w == page.w && page_cache_h == page.h) {
		blit_page(page, page_backbuf, pagebuf_w);
		return;
	}
	ngui_fill_rect(&ui, page, ui.theme.bg);
	if (!doc.nnode || (page_loading && doc.nnode <= 1)) {
		const char *s = page_loading ? "Loading..." : "No page";
		int w = (int)ngui_text_width_ui(&ui, s);
		ngui_text(&ui, page.x + (page.w - w) / 2,
			  page.y + page.h / 2 - 8, s, ui.theme.text_dim);
		page_scroll.content_h = 0;
		page_scroll.view_h = page.h;
		page_cache_valid = 0;
		return;
	}
	if (page.w < 40 || page.h < 40) {
		page_cache_valid = 0;
		return;
	}

	saved_px = ui.px;
	saved_w = ui.w;
	saved_h = ui.h;
	use_buf = page_backbuf && pagebuf_w == ui.w && pagebuf_h == ui.h;
	if (use_buf) {
		ui.px = page_backbuf;
		ui.w = pagebuf_w;
		ui.h = pagebuf_h;
		ngui_fill_rect(&ui, page, ui.theme.bg);
	}

	left = page.x + 8;
	right = page.x + page.w - 16;
	if (!layout_valid || left != layout_left || right != layout_right ||
	    layout_nnode != doc.nnode) {
		layout_content_h = measure_page(left, right);
		layout_valid = 1;
		layout_left = left;
		layout_right = right;
		layout_nnode = doc.nnode;
	}
	ch = layout_content_h;
	page_scroll.content_h = ch;
	page_scroll.view_h = page.h;
	ngui_scroll_begin(&ui, page, &page_scroll);
	render_page(left, right, page.y + 4 - page_scroll.offset);
	old_panel = ui.theme.panel_alt;
	old_dim = ui.theme.text_dim;
	ui.theme.panel_alt = 0xC618;
	ui.theme.text_dim = 0x8410;
	ngui_scroll_end(&ui, page, &page_scroll);
	ui.theme.panel_alt = old_panel;
	ui.theme.text_dim = old_dim;

	if (use_buf) {
		ui.px = saved_px;
		ui.w = saved_w;
		ui.h = saved_h;
		blit_page(page, page_backbuf, pagebuf_w);
		page_cache_valid = 1;
		page_cache_scroll = page_scroll.offset;
		page_cache_nnode = doc.nnode;
		page_cache_x = page.x;
		page_cache_y = page.y;
		page_cache_w = page.w;
		page_cache_h = page.h;
	} else {
		page_cache_valid = 0;
	}
}

static void draw_status(struct ngui_rect r)
{
	char msg[BR_URL_MAX + 32];
	unsigned int n = 0;
	int hover = page_hover_node();
	unsigned short color;
	const char *kind;

	ngui_fill_rect(&ui, r, ui.theme.panel);
	ngui_fill_rect(&ui, (struct ngui_rect){r.x, r.y, r.w, 1},
		       ui.theme.border);
	if (hover) {
		const char *href = br_href(&doc, (unsigned int)hover);
		char url[BR_URL_MAX];
		if (resolve_link(page_url, href, url, sizeof(url)) == 0) {
			s_append(msg, &n, "Link: ", sizeof(msg));
			s_append(msg, &n, url, sizeof(msg));
		} else {
			s_copy(msg, "Link", sizeof(msg));
		}
		color = ui.theme.accent;
	} else if (status_text[0]) {
		s_copy(msg, status_text, sizeof(msg));
		color = status_error ? ui.theme.error : ui.theme.text;
	} else if (page_title[0]) {
		s_copy(msg, page_title, sizeof(msg));
		color = ui.theme.text;
	} else {
		s_copy(msg, "Ready", sizeof(msg));
		color = ui.theme.text_dim;
	}
	kind = s_eq(page_url, "about:home") ? "HOME" :
	       page_is_local ? "FILE" :
	       (starts_with(page_url, "https://") ? "HTTPS" : "HTTP");
	ngui_text_ellipsis(&ui, r, r.x + 6, r.y + 1, msg, color);
	{
		int kw = (int)ngui_text_width_ui(&ui, kind);
		ngui_text(&ui, r.x + r.w - 6 - kw, r.y + 1, kind,
			  ui.theme.text_dim);
	}
}

static void draw(unsigned int *action)
{
	struct ngui_rect r = {0, 0, ui.w, ui.h};
	struct ngui_rect toolbar, status, page;

	toolbar = ngui_cut_top(&r, BR_TOOLBAR_H);
	status = ngui_cut_bottom(&r, BR_STATUS_H);
	page = r;
	draw_toolbar(toolbar, action);
	draw_page(page);
	draw_status(status);
}

static void redraw_now(void)
{
	unsigned int action = 0;

	ngui_begin(&ui);
	draw(&action);
	ngui_end(&ui);
}

static void handle_keys(unsigned int *action)
{
	int k, scrolled = 0;

	/* Batch queued page keys: a slow full-page render must not make a
	 * held Page Down advance only once every few frames. */
	while (scrolled < 3 && ngui_scroll_keys(&ui, &page_scroll))
		scrolled++;
	if (scrolled)
		ngui_invalidate(&ui);
	if (ngui_focus(&ui))
		return;
	while ((k = ngui_key(&ui)) != 0) {
		switch (k) {
		case NGUI_KEY_ESC:
			*action |= ACT_CLOSE;
			break;
		case NGUI_KEY_TAB:
		case 0x0C:              /* Ctrl+L */
			ngui_set_focus(&ui, ID_URL);
			break;
		case 0x12:              /* Ctrl+R */
			*action |= ACT_RELOAD;
			break;
		case NGUI_KEY_BACKSPACE:
			*action |= ACT_BACK;
			break;
		case NGUI_KEY_UP:
			ngui_scroll_to(&page_scroll,
				       page_scroll.offset - 24);
			ngui_invalidate(&ui);
			break;
		case NGUI_KEY_DOWN:
			ngui_scroll_to(&page_scroll,
				       page_scroll.offset + 24);
			ngui_invalidate(&ui);
			break;
		case NGUI_KEY_HOME:
			ngui_scroll_to(&page_scroll, 0);
			ngui_invalidate(&ui);
			break;
		case NGUI_KEY_END:
			ngui_scroll_to(&page_scroll,
				       page_scroll.content_h -
				       page_scroll.view_h);
			ngui_invalidate(&ui);
			break;
		default:
			break;
		}
	}
}

static void handle_page_click(unsigned int *action)
{
	int node;
	char url[BR_URL_MAX];

	if (!ui.pressed)
		return;
	if (ui.press_x < page_rect.x ||
	    ui.press_x >= page_rect.x + page_rect.w - 12 ||
	    ui.press_y < page_rect.y ||
	    ui.press_y >= page_rect.y + page_rect.h)
		return;
	node = find_hit(ui.press_x, ui.press_y);
	if (!node)
		return;
	if (resolve_link(page_url, br_href(&doc, (unsigned int)node), url,
			 sizeof(url)) != 0)
		return;
	s_copy(click_url, url, sizeof(click_url));
	*action |= ACT_NAV;
}

void _start(const char *arg)
{
	struct win_info wi;

	if (u_win_create(BR_W, BR_H, "Browser", &wi) != 0 &&
	    u_win_create(960, 640, "Browser", &wi) != 0 &&
	    u_win_create(780, 540, "Browser", &wi) != 0) {
		u_puts("browser: no graphics window\n");
		u_exit(1);
	}
	ngui_init(&ui, (unsigned short *)wi.fb, (int)wi.width, (int)wi.height);
	url_in.placeholder = "http://...";
	br_doc_init(&doc);
	br_set_theme(ui.theme.text, ui.theme.bg, ui.theme.panel,
		     ui.theme.accent);
	br_set_viewport(wi.width);
	if (u_getcwd(cwd, sizeof(cwd)) != 0 || !cwd[0])
		s_copy(cwd, "/", sizeof(cwd));
	heap_clear();
	origin_cache_load();
	css_cache_init();
	script_cache_init();
	page_scroll.offset = 0;
	page_scroll.content_h = 0;

	if (arg && arg[0]) {
		navigate(arg, 1);
	} else {
		load_home(1);
		/* Start ready to type: keep the home page, but leave the address
		 * bar empty instead of showing "about:home". */
		ngui_input_set(&url_in, "");
		ngui_set_focus(&ui, ID_URL);
	}

	for (;;) {
		unsigned int action = 0;
		unsigned int now;

		click_url[0] = '\0';
		if (pending_action) {
			action |= pending_action & ACT_WAIT_NAV;
			if (pending_url[0])
				s_copy(click_url, pending_url, sizeof(click_url));
			pending_action = 0;
			pending_url[0] = '\0';
			cancel_fetch = 0;
		} else if (preload_active) {
			if (preload_wait)
				preload_wait = 0;
			else
				preload_step();
		} else if (preload_pending &&
			   page_scroll.offset != preload_scroll) {
			preload_scroll = page_scroll.offset;
			preload_pending = 0;
			preload_node = 0;
			preload_active = 1;
		}
		js_browser_tick();
		if (pending_action)
			continue;
		now = u_ticks_ms();
		if (status_until && (int)(now - status_until) >= 0) {
			status_text[0] = '\0';
			status_until = 0;
			status_error = 0;
			ngui_invalidate(&ui);
		}
		if (!ngui_begin(&ui)) {
			/* A queued navigation (e.g. Enter in the URL bar during
			 * a slow fetch) must be handled even when this frame
			 * needs no redraw. */
			if (!action) {
				u_sleep_ms(10);
				continue;
			}
		} else {
			draw(&action);
			handle_keys(&action);
			handle_page_click(&action);
			ngui_end(&ui);
		}
		if (action & ACT_NAV) {
			if (click_url[0]) {
				navigate(click_url, 1);
				click_url[0] = '\0';
			} else {
				navigate(url_in.text, 1);
			}
		}
		if (action & ACT_BACK) {
			if (hist_pos > 0) {
				hist_pos--;
				navigate(history[hist_pos], 0);
			}
		}
		if (action & ACT_FORWARD) {
			if (hist_pos + 1 < hist_len) {
				hist_pos++;
				navigate(history[hist_pos], 0);
			}
		}
		if (action & ACT_RELOAD) {
			if (s_eq(page_url, "about:home"))
				load_home(0);
			else
				navigate(page_url, 0);
		}
		if (action & ACT_HOME)
			load_home(1);
		if (action & ACT_CLOSE)
			quit = 1;
		if (quit)
			break;
		if (pending_action)
			continue;
		u_sleep_ms(10);
	}
	fetch_helper_stop();
	u_win_close();
	u_exit(0);
}
