/* browser_html: see browser_html.h.  A deliberately tolerant parser: bad
 * nesting is repaired, unknown tags are transparent, and the CSS engine only
 * implements the properties in the browser's documented subset. */

#include "browser_html.h"

static int is_space(char c);
static int is_ident(char c);

/* ---- storage (one document per process) ---- */

static struct br_node nodes_store[BR_MAX_NODES];
static char text_store[BR_TEXT_SIZE];
static char attr_store[BR_ATTR_SIZE];
static struct br_rule rule_fallback[BR_RULE_FALLBACK];
static char css_store[BR_CSS_SIZE];
static int root_font_px = 16;

/* ---- tag table ---- */

struct tag_name {
	const char *name;
	unsigned char tag;
};

static const struct tag_name tag_names[] = {
	{ "html", BR_TAG_HTML }, { "head", BR_TAG_HEAD },
	{ "body", BR_TAG_BODY }, { "title", BR_TAG_TITLE },
	{ "style", BR_TAG_STYLE }, { "script", BR_TAG_SCRIPT },
	{ "meta", BR_TAG_META }, { "link", BR_TAG_LINK },
	{ "base", BR_TAG_BASE }, { "noscript", BR_TAG_NOSCRIPT },
	{ "p", BR_TAG_P }, { "div", BR_TAG_DIV }, { "span", BR_TAG_SPAN },
	{ "a", BR_TAG_A }, { "b", BR_TAG_B }, { "strong", BR_TAG_STRONG },
	{ "i", BR_TAG_I }, { "em", BR_TAG_EM }, { "u", BR_TAG_U },
	{ "code", BR_TAG_CODE }, { "font", BR_TAG_FONT },
	{ "small", BR_TAG_SMALL }, { "big", BR_TAG_BIG },
	{ "br", BR_TAG_BR }, { "hr", BR_TAG_HR }, { "img", BR_TAG_IMG },
	{ "picture", BR_TAG_PICTURE },
	{ "h1", BR_TAG_H1 }, { "h2", BR_TAG_H2 }, { "h3", BR_TAG_H3 },
	{ "h4", BR_TAG_H4 }, { "h5", BR_TAG_H5 }, { "h6", BR_TAG_H6 },
	{ "ul", BR_TAG_UL }, { "ol", BR_TAG_OL }, { "li", BR_TAG_LI },
	{ "blockquote", BR_TAG_BLOCKQUOTE }, { "pre", BR_TAG_PRE },
	{ "center", BR_TAG_CENTER }, { "table", BR_TAG_TABLE },
	{ "thead", BR_TAG_THEAD }, { "tbody", BR_TAG_TBODY },
	{ "tfoot", BR_TAG_TFOOT }, { "tr", BR_TAG_TR },
	{ "td", BR_TAG_TD }, { "th", BR_TAG_TH },
	{ "caption", BR_TAG_CAPTION }, { "form", BR_TAG_FORM },
	{ "input", BR_TAG_INPUT }, { "button", BR_TAG_BUTTON },
	{ "label", BR_TAG_LABEL }, { "select", BR_TAG_SELECT },
	{ "option", BR_TAG_OPTION }, { "textarea", BR_TAG_TEXTAREA },
	{ "iframe", BR_TAG_IFRAME }, { "svg", BR_TAG_SVG },
	{ "video", BR_TAG_VIDEO }, { "audio", BR_TAG_AUDIO },
	{ "canvas", BR_TAG_CANVAS }, { "object", BR_TAG_OBJECT },
	{ "main", BR_TAG_MAIN }, { "header", BR_TAG_HEADER },
	{ "footer", BR_TAG_FOOTER }, { "nav", BR_TAG_NAV },
	{ "section", BR_TAG_SECTION }, { "article", BR_TAG_ARTICLE },
	{ "aside", BR_TAG_ASIDE }, { "figure", BR_TAG_FIGURE },
	{ "figcaption", BR_TAG_FIGCAPTION }, { "details", BR_TAG_DETAILS },
	{ "summary", BR_TAG_SUMMARY }, { "dl", BR_TAG_DL },
	{ "dt", BR_TAG_DT }, { "dd", BR_TAG_DD },
	{ "address", BR_TAG_ADDRESS }, { "mark", BR_TAG_MARK },
	{ "time", BR_TAG_TIME }, { "abbr", BR_TAG_ABBR },
	{ "cite", BR_TAG_CITE }, { "q", BR_TAG_Q }, { "s", BR_TAG_S },
	{ "strike", BR_TAG_STRIKE }, { "sub", BR_TAG_SUB },
	{ "sup", BR_TAG_SUP }, { "bgsound", BR_TAG_BGSOUND },
	{ "embed", BR_TAG_EMBED }, { "param", BR_TAG_PARAM },
	{ "source", BR_TAG_SOURCE }, { "track", BR_TAG_TRACK },
	{ "wbr", BR_TAG_WBR }, { "area", BR_TAG_AREA },
	{ "col", BR_TAG_COL }, { "colgroup", BR_TAG_COLGROUP },
	{ 0, 0 }
};

const char *br_tag_name(unsigned int tag)
{
	unsigned int i;

	for (i = 0; tag_names[i].name; i++)
		if (tag_names[i].tag == tag)
			return tag_names[i].name;
	return "?";
}

static unsigned int s_len(const char *s);

#define TAG_HASH_SIZE 128

static unsigned char tag_hash_idx[TAG_HASH_SIZE];
static int tag_hash_ready;

static unsigned int tag_hash_key(const char *name, unsigned int len)
{
	unsigned int i, h = 2166136261u;

	for (i = 0; i < len; i++) {
		h ^= (unsigned char)name[i];
		h *= 16777619u;
	}
	return h & (TAG_HASH_SIZE - 1);
}

static void tag_hash_build(void)
{
	unsigned int i;

	for (i = 0; i < TAG_HASH_SIZE; i++)
		tag_hash_idx[i] = 0xFF;
	for (i = 0; tag_names[i].name; i++) {
		unsigned int h = tag_hash_key(tag_names[i].name,
					      s_len(tag_names[i].name));

		while (tag_hash_idx[h] != 0xFF)
			h = (h + 1) & (TAG_HASH_SIZE - 1);
		tag_hash_idx[h] = (unsigned char)i;
	}
	tag_hash_ready = 1;
}

unsigned int br_tag_from_name(const char *name)
{
	char lower[16];
	unsigned int i, len, h;
	unsigned char idx;

	for (i = 0; i < 15 && name[i]; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		lower[i] = c;
	}
	lower[i] = '\0';
	len = i;
	if (!tag_hash_ready)
		tag_hash_build();
	h = tag_hash_key(lower, len);
	for (;;) {
		idx = tag_hash_idx[h];
		if (idx == 0xFF)
			return BR_TAG_UNKNOWN;
		{
			const char *n = tag_names[idx].name;
			unsigned int j;

			for (j = 0; lower[j] && n[j] == lower[j]; j++)
				;
			if (!lower[j] && !n[j])
				return tag_names[idx].tag;
		}
		h = (h + 1) & (TAG_HASH_SIZE - 1);
	}
}

int br_tag_is_void(unsigned int tag)
{
	switch (tag) {
	case BR_TAG_BR: case BR_TAG_HR: case BR_TAG_IMG:
	case BR_TAG_META: case BR_TAG_LINK: case BR_TAG_BASE:
	case BR_TAG_INPUT: case BR_TAG_EMBED: case BR_TAG_PARAM:
	case BR_TAG_SOURCE: case BR_TAG_TRACK: case BR_TAG_WBR:
	case BR_TAG_AREA: case BR_TAG_COL:
		return 1;
	default:
		return 0;
	}
}

int br_tag_is_block(unsigned int tag)
{
	switch (tag) {
	case BR_TAG_HTML: case BR_TAG_BODY: case BR_TAG_P:
	case BR_TAG_DIV: case BR_TAG_H1: case BR_TAG_H2: case BR_TAG_H3:
	case BR_TAG_H4: case BR_TAG_H5: case BR_TAG_H6:
	case BR_TAG_UL: case BR_TAG_OL: case BR_TAG_LI:
	case BR_TAG_BLOCKQUOTE: case BR_TAG_PRE: case BR_TAG_CENTER:
	case BR_TAG_TABLE: case BR_TAG_THEAD: case BR_TAG_TBODY:
	case BR_TAG_TFOOT: case BR_TAG_TR: case BR_TAG_CAPTION:
	case BR_TAG_FORM: case BR_TAG_MAIN: case BR_TAG_HEADER:
	case BR_TAG_FOOTER: case BR_TAG_NAV: case BR_TAG_SECTION:
	case BR_TAG_ARTICLE: case BR_TAG_ASIDE: case BR_TAG_FIGURE:
	case BR_TAG_FIGCAPTION: case BR_TAG_DETAILS: case BR_TAG_SUMMARY:
	case BR_TAG_PICTURE:
	case BR_TAG_DL: case BR_TAG_DT: case BR_TAG_DD:
	case BR_TAG_ADDRESS: case BR_TAG_HR:
		return 1;
	default:
		return 0;
	}
}

/* ---- string / entity helpers ---- */

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

static int ci_eq_n(const char *a, const char *b, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		char ca = a[i], cb = b[i];

		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
			return 0;
	}
	return 1;
}

static int find_ci(const char *hay, unsigned int len, const char *needle,
		   unsigned int nlen, unsigned int from)
{
	unsigned int i;

	if (nlen == 0 || from >= len)
		return -1;
	for (i = from; i + nlen <= len; i++)
		if (ci_eq_n(hay + i, needle, nlen))
			return (int)i;
	return -1;
}

static const struct {
	const char *name;
	const char *text;
} entities[] = {
	{ "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
	{ "apos", "'" }, { "nbsp", " " }, { "copy", "(c)" },
	{ "reg", "(r)" }, { "trade", "(tm)" }, { "hellip", "..." },
	{ "mdash", "-" }, { "ndash", "-" }, { "laquo", "<<" },
	{ "raquo", ">>" }, { "bull", "*" }, { "middot", "." },
	{ "times", "x" }, { "divide", "/" }, { "deg", "o" },
	{ "plusmn", "+-" }, { "euro", "EUR" }, { "pound", "GBP" },
	{ "yen", "JPY" }, { "cent", "c" }, { "sect", "S" },
	{ "para", "P" }, { "frac12", "1/2" }, { "frac14", "1/4" },
	{ "frac34", "3/4" }, { "sup2", "2" }, { "sup3", "3" },
	{ "micro", "u" }, { "alpha", "a" }, { "beta", "b" },
	{ "pi", "p" }, { "larr", "<-" }, { "rarr", "->" },
	{ 0, 0 }
};

static void emit(char *out, unsigned int cap, unsigned int *n, char c)
{
	if (*n + 1 < cap)
		out[(*n)++] = c;
}

static void decode_entity(const char *s, unsigned int len, unsigned int *pos,
			  char *out, unsigned int cap, unsigned int *n)
{
	unsigned int start = *pos + 1, i = start;
	unsigned int code = 0;
	int ok = 0;

	while (i < len && s[i] != ';' && i - start < 12)
		i++;
	if (i >= len || s[i] != ';') {
		/* Not an entity: emit the '&' and advance past it.  Leaving
		 * *pos unchanged here used to spin forever on bare '&'. */
		emit(out, cap, n, '&');
		*pos = start;
		return;
	}
	if (s[start] == '#') {
		unsigned int j = start + 1;

		if (j < i && (s[j] == 'x' || s[j] == 'X')) {
			j++;
			for (; j < i; j++) {
				char c = s[j];
				int d = -1;
				if (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				if (d < 0) break;
				code = code * 16 + (unsigned int)d;
				ok = 1;
			}
		} else {
			for (; j < i; j++) {
				if (s[j] < '0' || s[j] > '9')
					break;
				code = code * 10 + (unsigned int)(s[j] - '0');
				ok = 1;
			}
		}
	} else {
		unsigned int k;

		for (k = 0; entities[k].name; k++) {
			unsigned int el = s_len(entities[k].name);
			unsigned int j;

			if (el != i - start)
				continue;
			for (j = 0; j < el; j++)
				if (s[start + j] != entities[k].name[j])
					break;
			if (j == el) {
				const char *t = entities[k].text;
				while (*t)
					emit(out, cap, n, *t++);
				*pos = i + 1;
				return;
			}
		}
	}
	if (ok && code) {
		if (code == 0x00A0)
			emit(out, cap, n, ' ');
		else if (code == 0x2013 || code == 0x2014)
			emit(out, cap, n, '-');
		else if (code == 0x2018 || code == 0x2019)
			emit(out, cap, n, '\'');
		else if (code == 0x201C || code == 0x201D)
			emit(out, cap, n, '"');
		else if (code == 0x2026) {
			emit(out, cap, n, '.'); emit(out, cap, n, '.');
			emit(out, cap, n, '.');
		} else if (code <= 0xFF) {
			emit(out, cap, n, (char)code);
		} else {
			emit(out, cap, n, '?');
		}
	} else {
		emit(out, cap, n, '?');
	}
	*pos = i + 1;
}

/* Decode UTF-8 and entities into out.  collapse folds whitespace runs to one
 * space and drops leading spaces.  Returns the output length. */
static unsigned int decode_text(const char *s, unsigned int len, char *out,
				unsigned int cap, int collapse)
{
	unsigned int i = 0, n = 0;

	while (i < len) {
		unsigned char c = (unsigned char)s[i];

		if (c == '&') {
			decode_entity(s, len, &i, out, cap, &n);
			continue;
		}
		if (c < 0x80) {
			i++;
			if (collapse && (c == ' ' || c == '\t' || c == '\n' ||
					 c == '\r' || c == '\f')) {
				if (n > 0 && out[n - 1] == ' ')
					continue;
				emit(out, cap, &n, ' ');
				continue;
			}
			if (!collapse && c == '\t') {
				emit(out, cap, &n, ' ');
				emit(out, cap, &n, ' ');
				emit(out, cap, &n, ' ');
				emit(out, cap, &n, ' ');
				continue;
			}
			emit(out, cap, &n, (char)c);
			continue;
		}
		/* UTF-8 -> Latin-1 where possible (the 8x16 font has those). */
		if ((c & 0xE0) == 0xC0 && i + 1 < len) {
			unsigned int cp = ((c & 0x1F) << 6) |
					  ((unsigned char)s[i + 1] & 0x3F);
			i += 2;
			if (cp == 0xA0)
				cp = ' ';
			else if (cp == 0xB7)
				cp = '-';
			else if (cp == 0xD7)
				cp = 'x';
			else if (cp == 0xB0)
				cp = 'o';
			if (cp <= 0xFF)
				emit(out, cap, &n, (char)cp);
			else
				emit(out, cap, &n, '?');
		} else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
			unsigned int cp = ((c & 0x0F) << 12) |
					  (((unsigned char)s[i + 1] & 0x3F) << 6) |
					  ((unsigned char)s[i + 2] & 0x3F);
			i += 3;
			if (cp == 0x2013 || cp == 0x2014) {
				emit(out, cap, &n, '-');
			} else if (cp == 0x2018 || cp == 0x2019) {
				emit(out, cap, &n, '\'');
			} else if (cp == 0x201C || cp == 0x201D) {
				emit(out, cap, &n, '"');
			} else if (cp == 0x2026) {
				emit(out, cap, &n, '.'); emit(out, cap, &n, '.');
				emit(out, cap, &n, '.');
			} else if (cp == 0x2192) {
				emit(out, cap, &n, '-'); emit(out, cap, &n, '>');
			} else if (cp == 0x2264) {
				emit(out, cap, &n, '<'); emit(out, cap, &n, '=');
			} else {
				emit(out, cap, &n, '?');
			}
		} else {
			i++;
			emit(out, cap, &n, '?');
		}
	}
	out[n] = '\0';
	return n;
}

/* Store an attribute string (entity-decoded) in the attr arena.  0 = no
 * space / empty. */
static unsigned int store_attr(struct br_doc *d, const char *s,
			       unsigned int len, unsigned int limit)
{
	unsigned int n;

	if (d->attrs_used + 2 >= BR_ATTR_SIZE)
		return 0;
	if (len > limit)
		len = limit;
	n = decode_text(s, len, d->attrs + d->attrs_used,
			BR_ATTR_SIZE - d->attrs_used, 0);
	if (n == 0)
		return 0;
	{
		unsigned int off = d->attrs_used;
		d->attrs_used += n + 1;
		return off;
	}
}

static const char *attr_ptr(struct br_doc *d, unsigned int off)
{
	return off ? d->attrs + off : "";
}

/* Copy a raw CSS string into the attr arena (no entity decoding). */
static unsigned int store_raw(struct br_doc *d, const char *s,
			      unsigned int len, unsigned int limit)
{
	unsigned int off;

	if (len > limit)
		len = limit;
	if (!len || d->attrs_used + len + 1 > BR_ATTR_SIZE)
		return 0;
	off = d->attrs_used;
	{
		unsigned int i;

		for (i = 0; i < len; i++)
			d->attrs[off + i] = s[i];
	}
	d->attrs[off + len] = '\0';
	d->attrs_used += len + 1;
	return off;
}

#define VAR_HASH_SIZE 1024

static unsigned int var_hash(const char *name, unsigned int len)
{
	unsigned int i, h = 2166136261u;

	for (i = 0; i < len; i++) {
		h ^= (unsigned char)name[i];
		h *= 16777619u;
	}
	return h & (VAR_HASH_SIZE - 1);
}

static unsigned int css_var_find(struct br_doc *d, const char *name,
				 unsigned int nlen)
{
	unsigned int h = var_hash(name, nlen);

	for (;;) {
		unsigned int i = d->var_slot[h];

		if (!i)
			return BR_MAX_VARS;
		i--;
		if (s_len(attr_ptr(d, d->var_name[i])) == nlen) {
			const char *n = attr_ptr(d, d->var_name[i]);
			unsigned int j;

			for (j = 0; j < nlen && n[j] == name[j]; j++)
				;
			if (j == nlen)
				return i;
		}
		h = (h + 1) & (VAR_HASH_SIZE - 1);
	}
}

static void css_var_set(struct br_doc *d, const char *name,
			unsigned int nlen, const char *val, unsigned int vlen)
{
	unsigned int i, noff, voff;

	if (nlen < 3 || name[0] != '-' || name[1] != '-' || !vlen)
		return;
	noff = store_raw(d, name, nlen, 63);
	voff = store_raw(d, val, vlen, 199);
	if (!noff || !voff)
		return;
	i = css_var_find(d, name, nlen);
	if (i >= BR_MAX_VARS) {
		unsigned int h;

		if (d->nvars >= BR_MAX_VARS)
			return;
		i = d->nvars++;
		d->var_name[i] = noff;
		h = var_hash(name, nlen);
		while (d->var_slot[h])
			h = (h + 1) & (VAR_HASH_SIZE - 1);
		d->var_slot[h] = (unsigned short)(i + 1);
	}
	d->var_val[i] = voff;
}

/* Find the end of a declaration value: the next ';' outside quotes and
 * parentheses.  The caller passes the position just after the ':'. */
static unsigned int css_value_end(const char *s, unsigned int len,
				  unsigned int i)
{
	int depth = 0;
	char quote = 0;

	while (i < len) {
		char c = s[i];

		if (quote) {
			if (c == quote)
				quote = 0;
			else if (c == '\\' && i + 1 < len)
				i++;
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		} else if (c == ';' && depth == 0) {
			break;
		}
		i++;
	}
	return i;
}

/* Theme-variant selectors (html.dark, .skin-theme-clientpref-night) carry a
 * second palette.  We render the light page, so skip their custom-property
 * definitions; otherwise their last-wins values leak into the light rules. */
static int css_dark_selector(const char *s, unsigned int len)
{
	unsigned int i;

	for (i = 0; i + 4 <= len; i++) {
		if ((s[i] == 'n' || s[i] == 'N') &&
		    (s[i + 1] == 'i' || s[i + 1] == 'I') &&
		    (s[i + 2] == 'g' || s[i + 2] == 'G') &&
		    (s[i + 3] == 'h' || s[i + 3] == 'H') &&
		    (s[i + 4] == 't' || s[i + 4] == 'T'))
			return 1;
		if ((s[i] == 'd' || s[i] == 'D') &&
		    (s[i + 1] == 'a' || s[i + 1] == 'A') &&
		    (s[i + 2] == 'r' || s[i + 2] == 'R') &&
		    (s[i + 3] == 'k' || s[i + 3] == 'K'))
			return 1;
		if (i + 12 <= len &&
		    (s[i] == 'c' || s[i] == 'C') &&
		    (s[i + 1] == 'l' || s[i + 1] == 'L') &&
		    (s[i + 2] == 'i' || s[i + 2] == 'I') &&
		    (s[i + 3] == 'e' || s[i + 3] == 'E') &&
		    (s[i + 4] == 'n' || s[i + 4] == 'N') &&
		    (s[i + 5] == 't' || s[i + 5] == 'T') &&
		    (s[i + 6] == 'p' || s[i + 6] == 'P') &&
		    (s[i + 7] == 'r' || s[i + 7] == 'R') &&
		    (s[i + 8] == 'e' || s[i + 8] == 'E') &&
		    (s[i + 9] == 'f' || s[i + 9] == 'F') &&
		    s[i + 10] == '-' &&
		    (s[i + 11] == 'o' || s[i + 11] == 'O') &&
		    (s[i + 12] == 's' || s[i + 12] == 'S'))
			return 1;
	}
	return 0;
}

/* Collect --custom-property definitions from one declaration block. */
static void css_collect_block(struct br_doc *d, const char *s,
			      unsigned int len)
{
	unsigned int i = 0;

	while (i < len) {
		unsigned int pstart, pend, vstart, vend;

		while (i < len && (is_space(s[i]) || s[i] == ';'))
			i++;
		if (i >= len)
			break;
		pstart = i;
		while (i < len && s[i] != ':' && s[i] != ';')
			i++;
		pend = i;
		if (i >= len || s[i] != ':') {
			if (i < len)
				i++;
			continue;
		}
		i++;
		vstart = i;
		i = css_value_end(s, len, i);
		vend = i;
		if (i < len)
			i++;
		while (pstart < pend && is_space(s[pstart]))
			pstart++;
		while (pend > pstart && is_space(s[pend - 1]))
			pend--;
		while (vstart < vend && is_space(s[vstart]))
			vstart++;
		while (vend > vstart && is_space(s[vend - 1]))
			vend--;
		if (vend - vstart >= 10 &&
		    ci_eq_n(s + vend - 10, "!important", 10)) {
			vend -= 10;
			while (vend > vstart && is_space(s[vend - 1]))
				vend--;
		}
		if (pend - pstart >= 3 && s[pstart] == '-' &&
		    s[pstart + 1] == '-')
			css_var_set(d, s + pstart, pend - pstart,
				    s + vstart, vend - vstart);
	}
}

const char *br_href(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].href_off);
}

const char *br_src(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].src_off);
}

const char *br_alt(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].alt_off);
}

const char *br_rel(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].rel_off);
}

const char *br_value(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].src_off);
}

const char *br_placeholder(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].alt_off);
}

const char *br_type(struct br_doc *d, unsigned int node)
{
	return attr_ptr(d, d->nodes[node].rel_off);
}

/* ---- document setup / node append ---- */

static void rule_index_reset(struct br_doc *d)
{
	unsigned int i;

	for (i = 0; i < BR_MAX_RULES; i++)
		d->rule_next[i] = 0xFFFF;
	for (i = 0; i < BR_RULE_HASH; i++)
		d->rule_bucket[i] = 0xFFFF;
	for (i = 0; i < BR_TAG_MAX; i++)
		d->rule_tag[i] = 0xFFFF;
	d->rule_any = 0xFFFF;
}

void br_doc_init(struct br_doc *d)
{
	unsigned int i;

	for (i = 0; i < sizeof(*d); i++)
		((unsigned char *)d)[i] = 0;
	rule_index_reset(d);
	d->nodes = nodes_store;
	d->text = text_store;
	d->attrs = attr_store;
	d->rules = rule_fallback;
	d->rule_cap = BR_RULE_FALLBACK;
	d->attrs_used = 1;      /* offset 0 = "no attribute" */
	d->attrs[0] = '\0';
	d->nvars = 0;
	d->root = 0;
	for (unsigned int vi = 0; vi < VAR_HASH_SIZE; vi++)
		d->var_slot[vi] = 0;
	root_font_px = 16;
}

void br_set_rule_store(struct br_doc *d, struct br_rule *buf,
		       unsigned int cap)
{
	if (!buf || !cap) {
		d->rules = rule_fallback;
		d->rule_cap = BR_RULE_FALLBACK;
		d->nrules = 0;
		return;
	}
	if (cap > BR_MAX_RULES)
		cap = BR_MAX_RULES;
	d->rules = buf;
	d->rule_cap = cap;
	d->nrules = 0;
}

static void doc_reset(struct br_doc *d)
{
	unsigned int i;

	d->nnode = 0;
	d->text_used = 0;
	d->attrs_used = 1;
	d->attrs[0] = '\0';
	d->nrules = 0;
	rule_index_reset(d);
	d->title[0] = '\0';
	d->inline_css_len = 0;
	d->nvars = 0;
	d->truncated = 0;
	d->plain = 0;
	for (i = 0; i < BR_MAX_NODES; i++) {
		d->nodes[i].type = BR_NODE_ELEM;
		d->nodes[i].tag = BR_TAG_UNKNOWN;
		d->nodes[i].flags = 0;
		d->nodes[i].parent = 0;
		d->nodes[i].first = d->nodes[i].last = 0;
		d->nodes[i].next = 0;
		d->nodes[i].text_off = d->nodes[i].text_len = 0;
		d->nodes[i].href_off = d->nodes[i].class_off = 0;
		d->nodes[i].id_off = d->nodes[i].style_off = 0;
		d->nodes[i].src_off = d->nodes[i].alt_off = 0;
		d->nodes[i].color_off = d->nodes[i].rel_off = 0;
		d->nodes[i].width_attr = d->nodes[i].height_attr = 0;
		d->nodes[i].name_off = 0;
		d->nodes[i].width_pct = d->nodes[i].height_pct = 0;
		d->nodes[i].svg_x = d->nodes[i].svg_y = 0;
		d->nodes[i].svg_rx = 0;
		d->nodes[i].img_index = -1;
		d->nodes[i].style_valid = 0;
	}
}

static unsigned int node_append(struct br_doc *d, unsigned int parent,
				unsigned char type, unsigned char tag)
{
	unsigned int idx;
	struct br_node *n;

	if (d->nnode >= BR_MAX_NODES) {
		d->truncated = 1;
		return 0;
	}
	idx = d->nnode++;
	n = &d->nodes[idx];
	n->type = type;
	n->tag = tag;
	n->parent = (unsigned short)parent;
	n->img_index = -1;
	if (d->nodes[parent].last)
		d->nodes[d->nodes[parent].last].next = (unsigned short)idx;
	else
		d->nodes[parent].first = (unsigned short)idx;
	d->nodes[parent].last = (unsigned short)idx;
	return idx;
}

static unsigned int append_text(struct br_doc *d, unsigned int parent,
				const char *s, unsigned int len, int pre)
{
	char *dst;
	unsigned int n;
	unsigned int idx;

	if (len == 0)
		return 0;
	if (d->text_used + 2 >= BR_TEXT_SIZE) {
		d->truncated = 1;
		return 0;
	}
	dst = d->text + d->text_used;
	n = decode_text(s, len, dst, BR_TEXT_SIZE - d->text_used, !pre);
	if (n == 0) {
		d->text[d->text_used] = '\0';
		return 0;
	}
	idx = node_append(d, parent, BR_NODE_TEXT, 0);
	if (!idx)
		return 0;
	d->nodes[idx].text_off = (unsigned short)d->text_used;
	d->nodes[idx].text_len = (unsigned short)n;
	d->nodes[idx].flags = pre ? BR_NODE_PREFORMAT : 0;
	d->text_used += n + 1;
	return idx;
}

/* ---- attribute parsing ---- */

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int str_eq(const char *a, const char *b);

static unsigned int parse_uint(const char *s, unsigned int len)
{
	unsigned int i = 0, v = 0;

	while (i < len && is_space(s[i]))
		i++;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (unsigned int)(s[i] - '0');
		if (v > 4096)
			v = 4096;
		i++;
	}
	return v;
}

static int contains_ci_len(const char *s, unsigned int len,
			   const char *needle);

static int parse_svg_viewbox(const char *s, unsigned int len,
			     unsigned short *w, unsigned short *h)
{
	unsigned int i = 0, v[4], n = 0;

	while (i < len && n < 4) {
		unsigned int x = 0;
		int any = 0;

		while (i < len && (is_space(s[i]) || s[i] == ','))
			i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			x = x * 10 + (unsigned int)(s[i] - '0');
			i++;
			any = 1;
		}
		if (!any) {
			if (i < len)
				i++;
			continue;
		}
		v[n++] = x;
	}
	if (n < 4)
		return 0;
	*w = (unsigned short)(v[2] > 4096 ? 4096 : v[2]);
	*h = (unsigned short)(v[3] > 4096 ? 4096 : v[3]);
	return *w && *h;
}

static void note_attr_flag(struct br_node *n, const char *name)
{
	if (str_eq(name, "data-collapse-target"))
		n->attr_flags |= BR_ATTR_COLLAPSE_TARGET;
	else if (str_eq(name, "data-collapse-trigger"))
		n->attr_flags |= BR_ATTR_COLLAPSE_TRIGGER;
	else if (str_eq(name, "is-open"))
		n->attr_flags |= BR_ATTR_IS_OPEN;
	else if (str_eq(name, "disabled"))
		n->attr_flags |= BR_ATTR_DISABLED;
	else if (str_eq(name, "open"))
		n->attr_flags |= BR_ATTR_OPEN;
	else if (str_eq(name, "inert"))
		n->attr_flags |= BR_ATTR_INERT;
}

/* Parse attributes from `s` (between the tag name and '>'). */
static void parse_attrs(struct br_doc *d, struct br_node *n,
			const char *s, unsigned int len)
{
	unsigned int i = 0;

	while (i < len) {
		char name[32];
		unsigned int nl = 0, vstart, vlen;
		char quote = 0;

		while (i < len && is_space(s[i]))
			i++;
		if (i >= len || s[i] == '/')
			break;
		while (i < len && !is_space(s[i]) && s[i] != '=' && s[i] != '>') {
			char c = s[i++];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			if (nl + 1 < sizeof(name))
				name[nl++] = c;
		}
		name[nl] = '\0';
		while (i < len && is_space(s[i]))
			i++;
		if (i >= len || s[i] != '=') {
			if (str_eq(name, "hidden"))
				n->flags |= BR_NODE_HIDDEN;
			note_attr_flag(n, name);
			continue;
		}
		i++;
		while (i < len && is_space(s[i]))
			i++;
		if (i < len && (s[i] == '"' || s[i] == '\'')) {
			quote = s[i++];
			vstart = i;
			while (i < len && s[i] != quote)
				i++;
			vlen = i - vstart;
			if (i < len)
				i++;
		} else {
			vstart = i;
			while (i < len && !is_space(s[i]) && s[i] != '>')
				i++;
			vlen = i - vstart;
		}
		if (!nl)
			continue;
		if (str_eq(name, "hidden")) {
			n->flags |= BR_NODE_HIDDEN;
			continue;
		}
		note_attr_flag(n, name);
		if (str_eq(name, "href"))
			n->href_off = store_attr(d, s + vstart, vlen, 480);
		else if (str_eq(name, "class"))
			n->class_off = store_attr(d, s + vstart, vlen, 120);
		else if (str_eq(name, "id"))
			n->id_off = store_attr(d, s + vstart, vlen, 64);
		else if (str_eq(name, "style"))
			n->style_off = store_attr(d, s + vstart, vlen, 240);
		else if (str_eq(name, "src"))
			n->src_off = store_attr(d, s + vstart, vlen, 400);
		else if (str_eq(name, "alt"))
			n->alt_off = store_attr(d, s + vstart, vlen, 120);
		else if (str_eq(name, "color"))
			n->color_off = store_attr(d, s + vstart, vlen, 32);
		else if (str_eq(name, "rel"))
			n->rel_off = store_attr(d, s + vstart, vlen, 64);
		else if (str_eq(name, "value"))
			n->src_off = store_attr(d, s + vstart, vlen, 220);
		else if (str_eq(name, "placeholder") &&
			 (n->tag == BR_TAG_INPUT || n->tag == BR_TAG_TEXTAREA))
			n->alt_off = store_attr(d, s + vstart, vlen, 120);
		else if (str_eq(name, "type") &&
			 (n->tag == BR_TAG_INPUT || n->tag == BR_TAG_BUTTON ||
			  n->tag == BR_TAG_SELECT || n->tag == BR_TAG_TEXTAREA))
			n->rel_off = store_attr(d, s + vstart, vlen, 40);
		else if (str_eq(name, "width")) {
			n->width_attr = (unsigned short)parse_uint(s + vstart, vlen);
			if (vlen && s[vstart + vlen - 1] == '%')
				n->width_pct = 1;
		} else if (str_eq(name, "height")) {
			n->height_attr = (unsigned short)parse_uint(s + vstart, vlen);
			if (vlen && s[vstart + vlen - 1] == '%')
				n->height_pct = 1;
		} else if (str_eq(name, "x") || str_eq(name, "x1")) {
			n->svg_x = (signed short)parse_uint(s + vstart, vlen);
		} else if (str_eq(name, "y") || str_eq(name, "y1")) {
			n->svg_y = (signed short)parse_uint(s + vstart, vlen);
		} else if (str_eq(name, "x2")) {
			n->width_attr = (unsigned short)parse_uint(s + vstart, vlen);
		} else if (str_eq(name, "y2")) {
			n->height_attr = (unsigned short)parse_uint(s + vstart, vlen);
		} else if (str_eq(name, "rx")) {
			n->svg_rx = (unsigned char)parse_uint(s + vstart, vlen);
		} else if (str_eq(name, "d")) {
			n->src_off = store_attr(d, s + vstart, vlen, 300);
		} else if (str_eq(name, "viewbox")) {
			unsigned short vw = 0, vh = 0;

			if (parse_svg_viewbox(s + vstart, vlen, &vw, &vh)) {
				n->width_attr = vw;
				n->height_attr = vh;
				n->flags |= BR_NODE_VIEWBOX;
			}
		} else if (str_eq(name, "text-anchor")) {
			if (contains_ci_len(s + vstart, vlen, "middle"))
				n->attr_flags |= BR_ATTR_ANCHOR_MIDDLE;
			else if (contains_ci_len(s + vstart, vlen, "end"))
				n->attr_flags |= BR_ATTR_ANCHOR_END;
		}
	}
}

static int str_eq(const char *a, const char *b)
{
	unsigned int i = 0;

	while (a[i] && b[i] && a[i] == b[i])
		i++;
	return a[i] == '\0' && b[i] == '\0';
}

/* ---- CSS ---- */

enum {
	PROP_COLOR, PROP_BG, PROP_FONT_PX, PROP_BOLD, PROP_ITALIC, PROP_MONO,
	PROP_ALIGN, PROP_UNDERLINE,
	PROP_DISPLAY, PROP_MT, PROP_MR, PROP_MB, PROP_ML,
	PROP_PT, PROP_PR, PROP_PB, PROP_PL,
	PROP_BORDER_W, PROP_BORDER_COLOR,
	PROP_RADIUS_TL, PROP_RADIUS_TR, PROP_RADIUS_BR, PROP_RADIUS_BL,
	PROP_LIST_NONE,
	PROP_WIDTH, PROP_WIDTH_KIND, PROP_MAXW, PROP_MAXW_KIND,
	PROP_HEIGHT, PROP_HEIGHT_KIND, PROP_LH_PCT, PROP_LH_PX, PROP_UPPER,
	PROP_FLOAT, PROP_FLEX_GROW, PROP_FLEX_COL, PROP_FLEX_WRAP,
	PROP_GRID_NCOLS, PROP_GRID_GAP, PROP_GRID_SLOT, PROP_GRID_AUTO,
	PROP_POS, PROP_LEFT, PROP_TOP, PROP_LEFT_SET, PROP_TOP_SET,
	PROP_VIS_HIDDEN, PROP_OPACITY_HIDDEN, PROP_OVERFLOW_HIDDEN,
	PROP_COUNT
};

struct color_name {
	const char *name;
	unsigned short rgb;
};

static const struct color_name color_names[] = {
	{ "black", 0x0000 }, { "white", 0xFFFF }, { "red", 0xF800 },
	{ "lime", 0x07E0 }, { "green", 0x0400 }, { "blue", 0x001F },
	{ "yellow", 0xFFE0 }, { "cyan", 0x07FF }, { "aqua", 0x07FF },
	{ "magenta", 0xF81F }, { "fuchsia", 0xF81F }, { "gray", 0x8410 },
	{ "grey", 0x8410 }, { "silver", 0xC618 }, { "maroon", 0x8000 },
	{ "olive", 0x8400 }, { "navy", 0x0010 }, { "purple", 0x8010 },
	{ "teal", 0x0410 }, { "orange", 0xFD20 }, { "brown", 0xA145 },
	{ "pink", 0xFE19 }, { "gold", 0xFEA0 }, { "lightgray", 0xD69A },
	{ "lightgrey", 0xD69A }, { "darkgray", 0x528A },
	{ "darkgrey", 0x528A }, { 0, 0 }
};

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int named_color(const char *s, unsigned int len, unsigned short *out)
{
	unsigned int i;

	for (i = 0; color_names[i].name; i++) {
		unsigned int j, n = s_len(color_names[i].name);

		if (n != len)
			continue;
		for (j = 0; j < n; j++) {
			char c = s[j];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			if (c != color_names[i].name[j])
				break;
		}
		if (j == n) {
			*out = color_names[i].rgb;
			return 1;
		}
	}
	return 0;
}

/* Parse #rgb/#rrggbb/rgb()/rgba()/a named colour.  Alpha is composited
 * over white; the render target is a white page. */
static int parse_color(const char *s, unsigned int len, unsigned short *out)
{
	unsigned int i = 0;

	while (i < len && is_space(s[i]))
		i++;
	if (i < len && s[i] == '#') {
		int d[6], nd = 0;

		i++;
		while (i < len && nd < 6 && hexval(s[i]) >= 0)
			d[nd++] = hexval(s[i++]);
		if (nd == 3) {
			unsigned int r = (unsigned int)d[0], g = (unsigned int)d[1];
			unsigned int b = (unsigned int)d[2];
			*out = (unsigned short)(((r * 17 >> 3) << 11) |
						((g * 17 >> 2) << 5) |
						(b * 17 >> 3));
			return 1;
		}
		if (nd >= 6) {
			unsigned int r = (unsigned int)(d[0] * 16 + d[1]);
			unsigned int g = (unsigned int)(d[2] * 16 + d[3]);
			unsigned int b = (unsigned int)(d[4] * 16 + d[5]);
			*out = (unsigned short)(((r >> 3) << 11) |
						((g >> 2) << 5) | (b >> 3));
			return 1;
		}
		return 0;
	}
	if (i + 3 <= len && ci_eq_n(s + i, "rgb", 3)) {
		unsigned int v[3], k = 0, a = 255;

		while (i < len && s[i] != '(')
			i++;
		if (i >= len)
			return 0;
		i++;
		while (k < 3 && i < len) {
			unsigned int x = 0;
			int percent = 0;

			while (i < len && (is_space(s[i]) || s[i] == ','))
				i++;
			if (i >= len || s[i] < '0' || s[i] > '9')
				return 0;
			while (i < len && s[i] >= '0' && s[i] <= '9') {
				x = x * 10 + (unsigned int)(s[i] - '0');
				i++;
			}
			if (i < len && s[i] == '%') {
				percent = 1;
				i++;
			}
			if (percent)
				x = x * 255 / 100;
			if (x > 255)
				x = 255;
			v[k++] = x;
		}
		/* Optional alpha in both syntaxes:
		 *   rgba(1, 2, 3, 0.5), rgb(1 2 3 / 50%) */
		{
			unsigned int whole = 0, frac = 0, scale = 1;
			int percent = 0;

			while (i < len && (is_space(s[i]) || s[i] == ','))
				i++;
			if (i < len && s[i] == '/') {
				i++;
				while (i < len && is_space(s[i]))
					i++;
			}
			if (i < len && s[i] >= '0' && s[i] <= '9') {
				while (i < len && s[i] >= '0' && s[i] <= '9') {
					whole = whole * 10 +
						(unsigned int)(s[i] - '0');
					i++;
				}
				if (i < len && s[i] == '.') {
					i++;
					while (i < len && s[i] >= '0' &&
					       s[i] <= '9') {
						if (scale < 1000) {
							frac = frac * 10 +
								(unsigned int)(s[i] - '0');
							scale *= 10;
						}
						i++;
					}
				}
				while (i < len && is_space(s[i]))
					i++;
				if (i < len && s[i] == '%')
					percent = 1;
				if (percent)
					a = whole > 100 ? 255 :
					    whole * 255 / 100;
				else if (whole >= 1)
					a = 255;
				else
					a = frac * 255 / scale;
			}
		}
		if (k == 3) {
			unsigned int r = v[0], g = v[1], b = v[2];

			if (a < 255) {
				r = (r * a + 255u * (255u - a)) / 255u;
				g = (g * a + 255u * (255u - a)) / 255u;
				b = (b * a + 255u * (255u - a)) / 255u;
			}
			*out = (unsigned short)(((r >> 3) << 11) |
						((g >> 2) << 5) | (b >> 3));
			return 1;
		}
		return 0;
	}
	{
		unsigned int j = i;

		while (i < len && !is_space(s[i]))
			i++;
		return named_color(s + j, i - j, out);
	}
}

/* Length values for margins/padding: px, ignore percentages/units. */
static unsigned int viewport_w;

static int parse_len(const char *s, unsigned int len, unsigned int *pos)
{
	unsigned int i = *pos;
	int sign = 1;
	long v = 0;            /* hundredths of a unit before conversion */
	int digits = 0, frac = 0, unit = 0;

	while (i < len && is_space(s[i]))
		i++;
	if (i < len && (s[i] == '-' || s[i] == '+')) {
		if (s[i] == '-')
			sign = -1;
		i++;
	}
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (s[i] - '0');
		digits = 1;
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			if (frac < 2) {
				v = v * 10 + (s[i] - '0');
				frac++;
			}
			i++;
		}
	}
	while (frac < 2) {
		v *= 10;
		frac++;
	}
	while (frac > 2) {
		v /= 10;
		frac--;
	}
	if (i < len && s[i] == '%')
		unit = 2;
	else if (i + 2 < len && s[i] == 'r' && s[i + 1] == 'e' &&
		 s[i + 2] == 'm')
		unit = 1;
	else if (i + 1 < len && s[i] == 'e' && s[i + 1] == 'm')
		unit = 3;
	while (i < len && (s[i] == 'p' || s[i] == 'x' || s[i] == '%' ||
			   s[i] == 'e' || s[i] == 'm' || s[i] == 'r' ||
			   s[i] == 'a' || s[i] == 'd'))
		i++;
	*pos = i;
	if (!digits)
		return 0;
	if (unit == 1)
		v *= root_font_px;
	else if (unit == 3)
		v *= 16;
	else if (unit == 2)
		v = v * (long)viewport_w / 100;
	v /= 100;
	v *= sign;
	if (v < -2048)
		v = -2048;
	if (v > 2048)
		v = 2048;
	return (int)v;
}

/* width/height: kind 0 = auto/none, 1 = px, 2 = percent. */
static int parse_size(const char *s, unsigned int len, unsigned short *val,
		      unsigned char *kind)
{
	unsigned int i = 0;
	unsigned long v = 0;
	int digits = 0, frac = 0, pct = 0, rem = 0, vw = 0, bad_unit = 0;

	*val = 0;
	*kind = 0;
	while (i < len && is_space(s[i]))
		i++;
	if (i + 4 <= len && ci_eq_n(s + i, "auto", 4))
		return 0;
	if (i + 4 <= len && ci_eq_n(s + i, "none", 4))
		return 0;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (unsigned int)(s[i] - '0');
		digits = 1;
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			if (frac < 2) {
				v = v * 10 + (unsigned int)(s[i] - '0');
				frac++;
			}
			i++;
		}
	}
	while (frac < 2) {
		v *= 10;
		frac++;
	}
	while (frac > 2) {
		v /= 10;
		frac--;
	}
	if (!digits)
		return 0;
	while (i < len && is_space(s[i]))
		i++;
	if (i < len && s[i] == '%')
		pct = 1;
	else if (i + 2 < len && s[i] == 'r' && s[i + 1] == 'e' &&
		 s[i + 2] == 'm')
		rem = 1;
	else if (i + 1 < len && s[i] == 'e' && s[i + 1] == 'm')
		rem = 2;
	else if (i + 1 < len && s[i] == 'v' && s[i + 1] == 'w')
		vw = 1;
	else if (i + 1 < len && s[i] == 'p' && s[i + 1] == 'x')
		; /* px: already the default scale */
	else if (i < len && (is_ident(s[i]) || s[i] == '#'))
		bad_unit = 1;
	if (bad_unit)
		return 0;
	if (rem == 1)
		v *= root_font_px;
	else if (rem == 2)
		v *= 16;
	v /= 100;
	if (vw)
		v = v * viewport_w / 100;
	if (pct) {
		if (v > 100)
			v = 100;
		*kind = 2;
	} else {
		if (v > 2048)
			v = 2048;
		*kind = 1;
	}
	*val = (unsigned short)v;
	return 1;
}

static int parse_font_px(const char *s, unsigned int len)
{
	unsigned int i = 0;
	int whole = 0, frac = 0, fscale = 1, digits = 0;
	int v1000, px;
	int is_px = 1, is_rem = 0, is_pct = 0;

	while (i < len && is_space(s[i]))
		i++;
	if (i + 5 <= len && ci_eq_n(s + i, "large", 5))
		return 24;
	if (i + 7 <= len && ci_eq_n(s + i, "x-large", 7))
		return 32;
	if (i + 5 <= len && ci_eq_n(s + i, "small", 5))
		return 14;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		whole = whole * 10 + (s[i] - '0');
		digits = 1;
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			if (fscale < 1000) {
				frac = frac * 10 + (s[i] - '0');
				fscale *= 10;
			}
			i++;
		}
	}
	if (!digits)
		return 0;
	while (i < len && is_space(s[i]))
		i++;
	if (i + 3 <= len && ci_eq_n(s + i, "rem", 3)) {
		is_rem = 1;
		is_px = 0;
	} else if (i + 2 <= len && ci_eq_n(s + i, "em", 2)) {
		is_rem = 1;
		is_px = 0;
	} else if (i < len && s[i] == '%') {
		is_pct = 1;
		is_px = 0;
	}
	v1000 = whole * 1000 + frac * 1000 / fscale;
	if (is_rem)
		px = v1000 * root_font_px / 1000;
	else if (is_pct)
		px = v1000 * root_font_px / 100000;
	else if (is_px)
		px = v1000 / 1000;
	else
		px = v1000 / 1000;
	if (px < 8)
		px = 8;
	if (px > 72)
		px = 72;
	return px;
}

static int parse_line_height(const char *s, unsigned int len,
			     unsigned short *px, unsigned char *pct)
{
	unsigned int i = 0;
	int whole = 0, frac = 0, fscale = 1, digits = 0;
	int v1000;
	int is_px = 0, is_pct = 0;

	*px = 0;
	*pct = 0;
	while (i < len && is_space(s[i]))
		i++;
	if (i + 6 <= len && ci_eq_n(s + i, "normal", 6))
		return 0;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		whole = whole * 10 + (s[i] - '0');
		digits = 1;
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9' && fscale < 1000) {
			frac = frac * 10 + (s[i] - '0');
			fscale *= 10;
			i++;
		}
	}
	if (!digits)
		return 0;
	while (i < len && is_space(s[i]))
		i++;
	if (i + 2 <= len && ci_eq_n(s + i, "px", 2))
		is_px = 1;
	else if (i < len && s[i] == '%')
		is_pct = 1;
	v1000 = whole * 1000 + frac * 1000 / fscale;
	if (is_px) {
		int v = v1000 / 1000;

		if (v < 8)
			v = 8;
		if (v > 160)
			v = 160;
		*px = (unsigned short)v;
	} else {
		int v = v1000 / 10;     /* unitless multiplier -> percent */

		if (is_pct)
			v = v1000 / 1000;
		if (v < 50)
			v = 50;
		if (v > 400)
			v = 400;
		*pct = (unsigned char)v;
	}
	return 1;
}

static int contains_ci_len(const char *s, unsigned int len, const char *needle)
{
	unsigned int i, j, n = s_len(needle);

	for (i = 0; i + n <= len; i++) {
		for (j = 0; j < n; j++) {
			char a = s[i + j];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (a != needle[j])
				break;
		}
		if (j == n)
			return 1;
	}
	return 0;
}

/* Pull a colour out of a border/background shorthand. */
static int find_color(const char *s, unsigned int len, unsigned short *c)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (s[i] == '#')
			return parse_color(s + i, len - i, c);
		if ((s[i] == 'r' || s[i] == 'R') &&
		    i + 3 <= len && ci_eq_n(s + i, "rgb", 3))
			return parse_color(s + i, len - i, c);
	}
	for (i = 0; i < len; ) {
		unsigned int st, e;

		while (i < len && (is_space(s[i]) || s[i] == ','))
			i++;
		st = i;
		while (i < len && !is_space(s[i]) && s[i] != ',')
			i++;
		e = i;
		if (e > st && parse_color(s + st, e - st, c))
			return 1;
	}
	return 0;
}

/* Background shorthands can hold several layers before the final solid
 * colour.  The colour is conventionally the last token (e.g. a stack of
 * gradients followed by var(--bg)), so take the rightmost colour rather
 * than the first gradient stop. */
static int find_color_last(const char *s, unsigned int len, unsigned short *c)
{
	int last = -1;
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (s[i] == '#')
			last = (int)i;
		else if ((s[i] == 'r' || s[i] == 'R') &&
			 i + 3 <= len && ci_eq_n(s + i, "rgb", 3))
			last = (int)i;
	}
	if (last >= 0)
		return parse_color(s + last, len - (unsigned int)last, c);
	while (i > 0) {
		unsigned int st, e;

		while (i > 0 && (is_space(s[i - 1]) || s[i - 1] == ',' ||
				 s[i - 1] == ')' || s[i - 1] == '('))
			i--;
		e = i;
		while (i > 0 && !is_space(s[i - 1]) && s[i - 1] != ',' &&
		       s[i - 1] != ')' && s[i - 1] != '(')
			i--;
		st = i;
		if (e > st && parse_color(s + st, e - st, c))
			return 1;
	}
	return 0;
}

static int parse_display(const char *s, unsigned int len)
{
	unsigned int i;

	while (len && is_space(s[0])) { s++; len--; }
	for (i = 0; i < len && !is_space(s[i]) && s[i] != ';'; i++)
		;
	if (i == 4 && ci_eq_n(s, "none", 4))
		return BR_DISPLAY_NONE;
	if (i == 5 && ci_eq_n(s, "block", 5))
		return BR_DISPLAY_BLOCK;
	if (i == 6 && ci_eq_n(s, "inline", 6))
		return BR_DISPLAY_INLINE;
	if (i == 12 && ci_eq_n(s, "inline-block", 12))
		return BR_DISPLAY_INLINE_BLOCK;
	if (i == 10 && ci_eq_n(s, "list-item", 10))
		return BR_DISPLAY_BLOCK;
	if (i == 4 && ci_eq_n(s, "flex", 4))
		return BR_DISPLAY_FLEX;
	if (i == 11 && ci_eq_n(s, "inline-flex", 11))
		return BR_DISPLAY_FLEX;
	if (i == 5 && ci_eq_n(s, "table", 5))
		return BR_DISPLAY_BLOCK;
	if (i == 10 && ci_eq_n(s, "table-cell", 10))
		return BR_DISPLAY_BLOCK;
	/* Grid shells are common on news/wiki pages, but the two-column
	 * heuristic is not reliable for their nested area templates yet.
	 * Collapse them to block flow so the content stays readable. */
	if (i == 4 && ci_eq_n(s, "grid", 4))
		return BR_DISPLAY_BLOCK;
	if (i == 11 && ci_eq_n(s, "inline-grid", 11))
		return BR_DISPLAY_BLOCK;
	return BR_DISPLAY_INLINE;
}

static void decl_add(struct br_decl *out, unsigned int *nout,
		     unsigned char prop, int val, int important)
{
	if (*nout >= BR_DECLS)
		return;
	out[*nout].prop = prop;
	out[*nout].val = (short)val;
	out[*nout].important = (unsigned char)(important != 0);
	(*nout)++;
}

static int parse_border_width(const char *s, unsigned int len)
{
	unsigned int i = 0;

	while (i < len) {
		while (i < len && is_space(s[i]))
			i++;
		if (i >= len)
			break;
		if (s[i] == '#') {
			while (i < len && !is_space(s[i]))
				i++;
			continue;
		}
		if (i + 3 <= len && ci_eq_n(s + i, "rgb", 3)) {
			while (i < len && s[i] != ')')
				i++;
			if (i < len)
				i++;
			continue;
		}
		if (s[i] >= '0' && s[i] <= '9') {
			int v = 0;

			while (i < len && s[i] >= '0' && s[i] <= '9') {
				v = v * 10 + (s[i] - '0');
				i++;
			}
			if (v < 0)
				v = 0;
			if (v > 3)
				v = 3;
			return v;
		}
		while (i < len && !is_space(s[i]))
			i++;
	}
	return -1;
}


/* Expand var(--name[, fallback]) references.  Returns the expanded length;
 * the input is copied verbatim when it contains no var(). */
static unsigned int css_expand_vars(struct br_doc *d, const char *s,
				    unsigned int len, char *out,
				    unsigned int cap, int depth)
{
	unsigned int i = 0, n = 0;

	if (depth > 8)
		return 0;
	while (i < len && n + 1 < cap) {
		unsigned int p, inner, close, comma, nlen, fstart;
		char quote;
		int parens;

		if (!(i + 4 <= len && ci_eq_n(s + i, "var(", 4))) {
			out[n++] = s[i++];
			continue;
		}
		p = i + 4;
		quote = 0;
		parens = 1;
		while (p < len && parens) {
			char c = s[p];

			if (quote) {
				if (c == quote)
					quote = 0;
				else if (c == '\\' && p + 1 < len)
					p++;
			} else if (c == '"' || c == '\'') {
				quote = c;
			} else if (c == '(') {
				parens++;
			} else if (c == ')') {
				parens--;
			}
			p++;
		}
		if (parens) {
			out[n++] = s[i++];
			continue;
		}
		close = p - 1;
		inner = i + 4;
		while (inner < close && is_space(s[inner]))
			inner++;
		comma = inner;
		parens = 0;
		quote = 0;
		while (comma < close) {
			char c = s[comma];

			if (quote) {
				if (c == quote)
					quote = 0;
			} else if (c == '"' || c == '\'') {
				quote = c;
			} else if (c == '(') {
				parens++;
			} else if (c == ')' && parens > 0) {
				parens--;
			} else if (c == ',' && parens == 0) {
				break;
			}
			comma++;
		}
		nlen = comma - inner;
		while (nlen && is_space(s[inner + nlen - 1]))
			nlen--;
		fstart = comma < close ? comma + 1 : close;
		while (fstart < close && is_space(s[fstart]))
			fstart++;
		if (nlen >= 3 && s[inner] == '-' && s[inner + 1] == '-') {
			unsigned int vi = css_var_find(d, s + inner, nlen);

			if (vi < BR_MAX_VARS) {
				const char *v = attr_ptr(d, d->var_val[vi]);
				unsigned int vn = css_expand_vars(d, v, s_len(v),
								  out + n,
								  cap - n, depth + 1);
				n += vn;
			} else if (fstart < close) {
				unsigned int vn = css_expand_vars(d, s + fstart,
								  close - fstart,
								  out + n, cap - n,
								  depth + 1);
				n += vn;
			}
		} else if (fstart < close) {
			unsigned int vn = css_expand_vars(d, s + fstart,
							  close - fstart,
							  out + n, cap - n,
							  depth + 1);
			n += vn;
		}
		i = p;
	}
	out[n] = '\0';
	return n;
}


static unsigned int grid_skip_sep(const char *s, unsigned int len,
				  unsigned int i)
{
	while (i < len && (is_space(s[i]) || s[i] == ','))
		i++;
	return i;
}

/* Parse one grid track (px, %, rem/em, fr, auto, or the second minmax()
 * argument).  Returns 1 on success. */
static int parse_grid_track(const char *s, unsigned int len,
			    unsigned int *pp, unsigned short *val,
			    unsigned char *kind)
{
	unsigned int i = grid_skip_sep(s, len, *pp);
	unsigned int whole = 0, frac = 0, scale = 1;
	int any = 0;

	if (i + 7 <= len && ci_eq_n(s + i, "minmax(", 7)) {
		unsigned int j = i + 7;

		while (j < len && s[j] != ',')
			j++;
		if (j >= len)
			return 0;
		i = j + 1;
		while (i < len && is_space(s[i]))
			i++;
	}
	if (i + 7 <= len && ci_eq_n(s + i, "repeat(", 7)) {
		int depth = 1;

		i += 7;
		while (i < len && depth) {
			if (s[i] == '(')
				depth++;
			else if (s[i] == ')')
				depth--;
			i++;
		}
		*pp = i;
		return 0;
	}
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		whole = whole * 10 + (unsigned int)(s[i] - '0');
		i++;
		any = 1;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			if (scale < 1000) {
				frac = frac * 10 + (unsigned int)(s[i] - '0');
				scale *= 10;
			}
			i++;
			any = 1;
		}
	}
	if (!any) {
		if (i + 4 <= len && ci_eq_n(s + i, "auto", 4)) {
			*val = 1;
			*kind = 3;
			*pp = i + 4;
			return 1;
		}
		return 0;
	}
	if (i + 2 <= len && ci_eq_n(s + i, "fr", 2)) {
		*val = (unsigned short)(whole ? whole : 1);
		*kind = 3;
		i += 2;
	} else if (i < len && s[i] == '%') {
		*val = (unsigned short)(whole > 1000 ? 1000 : whole);
		*kind = 2;
		i++;
	} else if (i + 3 <= len && ci_eq_n(s + i, "rem", 3)) {
		*val = (unsigned short)(whole * 16 + frac * 16 / scale);
		*kind = 1;
		i += 3;
	} else if (i + 2 <= len && ci_eq_n(s + i, "em", 2)) {
		*val = (unsigned short)(whole * 16 + frac * 16 / scale);
		*kind = 1;
		i += 2;
	} else {
		*val = (unsigned short)(whole > 1000 ? 1000 : whole);
		*kind = 1;
		if (i + 2 <= len && ci_eq_n(s + i, "px", 2))
			i += 2;
	}
	while (i < len && s[i] != ')' && !is_space(s[i]) && s[i] != ',')
		i++;
	*pp = i;
	return 1;
}

static int parse_grid_cols(const char *s, unsigned int len,
			   unsigned short val[2], unsigned char kind[2])
{
	unsigned int pos = 0;
	int n = 0;

	while (n < 2) {
		unsigned int before = pos;

		if (!parse_grid_track(s, len, &pos, &val[n], &kind[n]))
			break;
		n++;
		if (pos <= before)
			break;
		while (pos < len && is_space(s[pos]))
			pos++;
		if (pos >= len)
			break;
	}
	return n;
}

static int parse_grid_slot(const char *s, unsigned int len)
{
	if (contains_ci_len(s, len, "footer") ||
	    contains_ci_len(s, len, "header") ||
	    contains_ci_len(s, len, "sitenotice") ||
	    contains_ci_len(s, len, "navbar"))
		return 1;
	if (contains_ci_len(s, len, "columnstart") ||
	    contains_ci_len(s, len, "sidebar") ||
	    contains_ci_len(s, len, "left") ||
	    contains_ci_len(s, len, "start"))
		return 2;
	if (contains_ci_len(s, len, "pagecontent") ||
	    contains_ci_len(s, len, "content") ||
	    contains_ci_len(s, len, "main") ||
	    contains_ci_len(s, len, "right") ||
	    contains_ci_len(s, len, "end"))
		return 3;
	return 0;
}

static int css_has_var(const char *s, unsigned int len)
{
	unsigned int i;

	for (i = 0; i + 4 <= len; i++)
		if ((s[i] == 'v' || s[i] == 'V') &&
		    ci_eq_n(s + i, "var(", 4))
			return 1;
	return 0;
}

static unsigned int css_skip_comment(const char *s, unsigned int len,
				     unsigned int i)
{
	i += 2;
	while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
		i++;
	if (i + 1 < len)
		i += 2;
	else
		i = len;
	return i;
}

static unsigned int css_skip_string(const char *s, unsigned int len,
				    unsigned int i)
{
	char q = s[i++];

	while (i < len && s[i] != q) {
		if (s[i] == '\\' && i + 1 < len)
			i++;
		i++;
	}
	if (i < len)
		i++;
	return i;
}

/* Find the '}' that closes a declaration block.  i points just past the
 * opening '{'.  Comments and quoted strings are skipped: a brace inside
 * one must not unbalance the scan (nd-aktuell's CSS comments contain
 * selectors such as "Highlight-Stories {" without a closing brace). */
static unsigned int css_block_end(const char *s, unsigned int len,
				  unsigned int i)
{
	int depth = 0;

	while (i < len) {
		char c = s[i];

		if (c == '/' && i + 1 < len && s[i + 1] == '*') {
			i = css_skip_comment(s, len, i);
			continue;
		}
		if (c == '"' || c == '\'') {
			i = css_skip_string(s, len, i);
			continue;
		}
		if (c == '{') {
			depth++;
		} else if (c == '}') {
			if (depth == 0)
				return i;
			depth--;
		}
		i++;
	}
	return len;
}

/* Skip a block whose opening '{' has already been consumed. */
static unsigned int css_skip_nested(const char *s, unsigned int len,
				    unsigned int i)
{
	int depth = 1;

	while (i < len && depth) {
		char c = s[i];

		if (c == '/' && i + 1 < len && s[i + 1] == '*') {
			i = css_skip_comment(s, len, i);
			continue;
		}
		if (c == '"' || c == '\'') {
			i = css_skip_string(s, len, i);
			continue;
		}
		if (c == '{')
			depth++;
		else if (c == '}')
			depth--;
		i++;
	}
	return i;
}

/* One border-radius length.  Unlike parse_size(), percentages keep their
 * kind: the drawer scales them to the box, not the viewport. */
static int parse_radius_value(const char *s, unsigned int len,
			      unsigned int *pos, unsigned short *val,
			      unsigned char *kind)
{
	unsigned int i = *pos;
	unsigned long v = 0;
	int digits = 0, fdigits = 0, frac = 0, pct = 0, rem = 0;

	while (i < len && is_space(s[i]))
		i++;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (unsigned int)(s[i] - '0');
		digits = 1;
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			fdigits = 1;
			if (frac < 2) {
				v = v * 10 + (unsigned int)(s[i] - '0');
				frac++;
			}
			i++;
		}
	}
	while (frac < 2) {
		v *= 10;
		frac++;
	}
	while (i < len && is_space(s[i]))
		i++;
	if (i < len && s[i] == '%') {
		pct = 1;
		i++;
	} else if (i + 2 < len && s[i] == 'r' && s[i + 1] == 'e' &&
		   s[i + 2] == 'm') {
		rem = 1;
		i += 3;
	} else if (i + 1 < len && s[i] == 'e' && s[i + 1] == 'm') {
		rem = 2;
		i += 2;
	} else if (i + 1 < len && s[i] == 'p' && s[i + 1] == 'x') {
		i += 2;
	} else if (i < len && is_ident(s[i])) {
		return 0;
	}
	if (!digits && !fdigits)
		return 0;
	if (rem == 1)
		v *= root_font_px;
	else if (rem == 2)
		v *= 16;
	v /= 100;
	if (v > 2048)
		v = 2048;
	*val = (unsigned short)v;
	*kind = pct ? 2 : 1;
	*pos = i;
	return 1;
}

static void parse_decls(struct br_doc *d, const char *s,
			unsigned int len, struct br_decl *out,
			unsigned int *nout)
{
	static char expanded[4096];

	if (css_has_var(s, len)) {
		unsigned int elen = css_expand_vars(d, s, len, expanded,
						   sizeof(expanded), 0);

		if (elen) {
			s = expanded;
			len = elen;
		}
	}
	unsigned int i = 0;

	*nout = 0;
	while (i < len && *nout < BR_DECLS) {
		unsigned int pstart, pend, vstart, vend, k;
		char prop[24];
		int important = 0;

		while (i < len && (is_space(s[i]) || s[i] == ';'))
			i++;
		if (i >= len)
			break;
		/* A nested at-rule (@media/@supports) inside the declaration
		 * block carries its own selector context; skip it rather than
		 * reading its prelude as a declaration. */
		if (s[i] == '@') {
			while (i < len && s[i] != '{' && s[i] != ';')
				i++;
			if (i < len && s[i] == '{')
				i = css_skip_nested(s, len, i + 1);
			else if (i < len)
				i++;
			continue;
		}
		pstart = i;
		while (i < len && s[i] != ':' && s[i] != ';' && s[i] != '{')
			i++;
		if (i < len && s[i] == '{') {
			/* Nested style rule: skip its block. */
			i = css_skip_nested(s, len, i + 1);
			continue;
		}
		pend = i;
		if (i >= len || s[i] != ':')
			continue;
		i++;
		vstart = i;
		while (i < len && s[i] != ';')
			i++;
		vend = i;
		while (pstart < pend && is_space(s[pstart]))
			pstart++;
		while (pend > pstart && is_space(s[pend - 1]))
			pend--;
		while (vstart < vend && is_space(s[vstart]))
			vstart++;
		while (vend > vstart && is_space(s[vend - 1]))
			vend--;
		if (vend - vstart >= 10 &&
		    ci_eq_n(s + vend - 10, "!important", 10)) {
			important = 1;
			vend -= 10;
			while (vend > vstart && is_space(s[vend - 1]))
				vend--;
		}
		for (k = 0; k < sizeof(prop) - 1 && pstart + k < pend; k++) {
			char c = s[pstart + k];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			prop[k] = c;
		}
		prop[k] = '\0';
		if (str_eq(prop, "color")) {
			unsigned short c;
			if (parse_color(s + vstart, vend - vstart, &c))
				decl_add(out, nout, PROP_COLOR, c, important);
		} else if (str_eq(prop, "fill")) {
			unsigned short c;

			if (parse_color(s + vstart, vend - vstart, &c)) {
				decl_add(out, nout, PROP_BG, c, important);
				decl_add(out, nout, PROP_COLOR, c, important);
			}
		} else if (str_eq(prop, "stroke")) {
			unsigned short c;

			if (parse_color(s + vstart, vend - vstart, &c)) {
				decl_add(out, nout, PROP_BORDER_COLOR, c,
					 important);
				decl_add(out, nout, PROP_BORDER_W, 1, important);
			}
		} else if (str_eq(prop, "stroke-width")) {
			unsigned int j = vstart;
			int sw = 0;

			while (j < vend && s[j] >= '0' && s[j] <= '9') {
				sw = sw * 10 + (s[j] - '0');
				j++;
			}
			if (sw < 1)
				sw = 1;
			if (sw > 3)
				sw = 3;
			decl_add(out, nout, PROP_BORDER_W, sw, important);
		} else if (str_eq(prop, "text-anchor")) {
			int a = BR_ALIGN_LEFT;

			if (contains_ci_len(s + vstart, vend - vstart, "middle"))
				a = BR_ALIGN_CENTER;
			else if (contains_ci_len(s + vstart, vend - vstart,
						 "end"))
				a = BR_ALIGN_RIGHT;
			decl_add(out, nout, PROP_ALIGN, a, important);
		} else if (str_eq(prop, "background-color") ||
			   str_eq(prop, "background")) {
			unsigned short c;
			if (parse_color(s + vstart, vend - vstart, &c))
				decl_add(out, nout, PROP_BG, c, important);
			else if (str_eq(prop, "background") &&
				 find_color_last(s + vstart, vend - vstart, &c))
				decl_add(out, nout, PROP_BG, c, important);
		} else if (str_eq(prop, "font-size")) {
			int px = parse_font_px(s + vstart, vend - vstart);
			if (px > 0)
				decl_add(out, nout, PROP_FONT_PX, px,
					 important);
		} else if (str_eq(prop, "font-weight")) {
			int bold = 0;
			unsigned int j;

			if (vend - vstart >= 4 &&
			    ci_eq_n(s + vstart, "bold", 4))
				bold = 1;
			else {
				int w = 0;
				for (j = vstart; j < vend; j++)
					if (s[j] >= '0' && s[j] <= '9')
						w = w * 10 + (s[j] - '0');
				if (w >= 500)
					bold = 1;
			}
			decl_add(out, nout, PROP_BOLD, bold, important);
		} else if (str_eq(prop, "font-style")) {
			int italic = (vend - vstart >= 6 &&
				      ci_eq_n(s + vstart, "italic", 6));

			decl_add(out, nout, PROP_ITALIC, italic, important);
		} else if (str_eq(prop, "font-family")) {
			unsigned int j;
			int mono = 0;

			for (j = vstart; j + 4 <= vend; j++)
				if (ci_eq_n(s + j, "mono", 4))
					mono = 1;
			decl_add(out, nout, PROP_MONO, mono, important);
		} else if (str_eq(prop, "text-align")) {
			int a = BR_ALIGN_LEFT;
			if (vend - vstart >= 6 &&
			    ci_eq_n(s + vstart, "center", 6))
				a = BR_ALIGN_CENTER;
			else if (vend - vstart >= 5 &&
				 ci_eq_n(s + vstart, "right", 5))
				a = BR_ALIGN_RIGHT;
			decl_add(out, nout, PROP_ALIGN, a, important);
		} else if (str_eq(prop, "text-decoration")) {
			int u = 0;
			if (vend - vstart >= 9 &&
			    ci_eq_n(s + vstart, "underline", 9))
				u = 1;
			decl_add(out, nout, PROP_UNDERLINE, u, important);
		} else if (str_eq(prop, "text-transform")) {
			int u = (vend - vstart >= 9 &&
				 ci_eq_n(s + vstart, "uppercase", 9));
			decl_add(out, nout, PROP_UPPER, u, important);
		} else if (str_eq(prop, "display")) {
			decl_add(out, nout, PROP_DISPLAY,
				 parse_display(s + vstart, vend - vstart),
				 important);
		} else if (str_eq(prop, "visibility")) {
			int h = contains_ci_len(s + vstart, vend - vstart,
						"hidden") ||
				contains_ci_len(s + vstart, vend - vstart,
						"collapse");
			decl_add(out, nout, PROP_VIS_HIDDEN, h, important);
		} else if (str_eq(prop, "opacity")) {
			unsigned int q;
			int h = 1, any = 0;

			for (q = vstart; q < vend; q++) {
				if (s[q] >= '1' && s[q] <= '9') {
					h = 0;
					break;
				}
				if (s[q] >= '0' && s[q] <= '9')
					any = 1;
			}
			if (!any || contains_ci_len(s + vstart, vend - vstart,
						    "inherit"))
				h = 0;
			decl_add(out, nout, PROP_OPACITY_HIDDEN, h, important);
		} else if (str_eq(prop, "overflow") ||
			   str_eq(prop, "overflow-x") ||
			   str_eq(prop, "overflow-y")) {
			int h = contains_ci_len(s + vstart, vend - vstart,
						"hidden") ||
				contains_ci_len(s + vstart, vend - vstart,
						"clip");
			decl_add(out, nout, PROP_OVERFLOW_HIDDEN, h, important);
		} else if (str_eq(prop, "float")) {
			int f = 0;

			if (vend - vstart >= 4 &&
			    ci_eq_n(s + vstart, "left", 4))
				f = 1;
			else if (vend - vstart >= 5 &&
				 ci_eq_n(s + vstart, "right", 5))
				f = 2;
			decl_add(out, nout, PROP_FLOAT, f, important);
		} else if (str_eq(prop, "flex-grow") ||
			   str_eq(prop, "flex")) {
			unsigned int q = vstart;
			int g = 0;

			while (q < vend && (s[q] == ' ' || s[q] == '\t'))
				q++;
			while (q < vend && s[q] >= '0' && s[q] <= '9') {
				g = g * 10 + (s[q] - '0');
				q++;
			}
			if (g > 100)
				g = 100;
			decl_add(out, nout, PROP_FLEX_GROW, g, important);
		} else if (str_eq(prop, "flex-direction")) {
			decl_add(out, nout, PROP_FLEX_COL,
				 contains_ci_len(s + vstart, vend - vstart,
						  "column"), important);
		} else if (str_eq(prop, "flex-wrap")) {
			int wrap = 0;

			if (contains_ci_len(s + vstart, vend - vstart, "nowrap"))
				wrap = 2;
			else if (contains_ci_len(s + vstart, vend - vstart,
						 "wrap"))
				wrap = 1;
			decl_add(out, nout, PROP_FLEX_WRAP, wrap, important);
		} else if (str_eq(prop, "position")) {
			int p = 0;

			if (contains_ci_len(s + vstart, vend - vstart,
					    "absolute"))
				p = 1;
			else if (contains_ci_len(s + vstart, vend - vstart,
						 "fixed"))
				p = 2;
			/* position:sticky stays in the flow.  Treating it as
			 * fixed painted the header over the first content
			 * block; a static header is safer than hidden text. */
			decl_add(out, nout, PROP_POS, p, important);
		} else if (str_eq(prop, "left") || str_eq(prop, "top")) {
			int is_left = prop[0] == 'l';
			unsigned int pos = 0;

			if (!(vend - vstart >= 4 &&
			      ci_eq_n(s + vstart, "auto", 4))) {
				int v = parse_len(s + vstart, vend - vstart, &pos);

				if (v > 32000)
					v = 32000;
				else if (v < -32000)
					v = -32000;
				decl_add(out, nout,
					 is_left ? PROP_LEFT : PROP_TOP, v, important);
				decl_add(out, nout,
					 is_left ? PROP_LEFT_SET : PROP_TOP_SET, 1,
					 important);
			}
		} else if (str_eq(prop, "grid-template-columns") ||
			   str_eq(prop, "grid-template")) {
			const char *v = s + vstart;
			unsigned int vl = vend - vstart;
			unsigned short val[2];
			unsigned char kind[2];
			int n, q;

			if (str_eq(prop, "grid-template")) {
				for (q = 0; q < (int)vl; q++)
					if (v[q] == '/') {
						q++;
						while (q < (int)vl && is_space(v[q]))
							q++;
						v += q;
						vl -= (unsigned int)q;
						break;
					}
			}
			if (contains_ci_len(v, vl, "auto-fit") ||
			    contains_ci_len(v, vl, "auto-fill")) {
				unsigned int mp = 0;
				int found = 0;
				unsigned short mw = 200;
				unsigned char mk = 1;

				for (q = 0; q + 7 <= (int)vl; q++)
					if (ci_eq_n(v + q, "minmax(", 7)) {
						mp = (unsigned int)q + 7;
						found = 1;
						break;
					}
				if (found)
					parse_grid_track(v, vl, &mp, &mw, &mk);
				if (mk != 1 || mw < 80)
					mw = 200;
				if (mw > 480)
					mw = 480;
				decl_add(out, nout, PROP_GRID_AUTO, 1, important);
				decl_add(out, nout, PROP_WIDTH, mw, important);
				decl_add(out, nout, PROP_WIDTH_KIND, 1, important);
			} else {
				n = parse_grid_cols(v, vl, val, kind);
				if (n > 0) {
					decl_add(out, nout, PROP_WIDTH, val[0],
						 important);
					decl_add(out, nout, PROP_WIDTH_KIND,
						 kind[0], important);
					decl_add(out, nout, PROP_GRID_NCOLS, n,
						 important);
				}
				if (n > 1) {
					decl_add(out, nout, PROP_MAXW, val[1],
						 important);
					decl_add(out, nout, PROP_MAXW_KIND,
						 kind[1], important);
				}
			}
		} else if (str_eq(prop, "gap") || str_eq(prop, "column-gap")) {
			unsigned int gp = 0;
			unsigned short gv = 0;
			unsigned char gk = 1;

			if (parse_grid_track(s, vend, &gp, &gv, &gk) && gk == 1) {
				if (gv > 120)
					gv = 120;
				decl_add(out, nout, PROP_GRID_GAP, gv, important);
			}
		} else if (str_eq(prop, "grid-area")) {
			int slot = parse_grid_slot(s + vstart, vend - vstart);

			if (slot)
				decl_add(out, nout, PROP_GRID_SLOT, slot, important);
		} else if (str_eq(prop, "width") ||
			   str_eq(prop, "max-width") ||
			   str_eq(prop, "height")) {
			unsigned short v;
			unsigned char kind;
			if (parse_size(s + vstart, vend - vstart, &v, &kind)) {
				if (str_eq(prop, "width")) {
					decl_add(out, nout, PROP_WIDTH, v, important);
					decl_add(out, nout, PROP_WIDTH_KIND, kind,
						 important);
				} else if (str_eq(prop, "max-width")) {
					decl_add(out, nout, PROP_MAXW, v, important);
					decl_add(out, nout, PROP_MAXW_KIND, kind,
						 important);
				} else {
					decl_add(out, nout, PROP_HEIGHT, v, important);
					decl_add(out, nout, PROP_HEIGHT_KIND, kind,
						 important);
				}
			}
		} else if (str_eq(prop, "line-height")) {
			unsigned short px;
			unsigned char pct;

			if (parse_line_height(s + vstart, vend - vstart, &px, &pct)) {
				if (px) {
					decl_add(out, nout, PROP_LH_PX, px, important);
					decl_add(out, nout, PROP_LH_PCT, 0, important);
				} else {
					decl_add(out, nout, PROP_LH_PX, 0, important);
					decl_add(out, nout, PROP_LH_PCT, pct, important);
				}
			}
		} else if (str_eq(prop, "list-style") ||
			   str_eq(prop, "list-style-type")) {
			int none = (vend - vstart >= 4 &&
				    contains_ci_len(s + vstart, vend - vstart,
						    "none"));
			decl_add(out, nout, PROP_LIST_NONE, none, important);
		} else if (str_eq(prop, "border-radius") ||
			   str_eq(prop, "border-top-left-radius") ||
			   str_eq(prop, "border-top-right-radius") ||
			   str_eq(prop, "border-bottom-right-radius") ||
			   str_eq(prop, "border-bottom-left-radius")) {
			unsigned short v[4];
			unsigned char k[4];
			unsigned int pos = 0;
			int n = 0;

			while (pos < vend - vstart && n < 4) {
				while (pos < vend - vstart &&
				       is_space(s[vstart + pos]))
					pos++;
				if (pos >= vend - vstart ||
				    s[vstart + pos] == '/')
					break;
				if (!parse_radius_value(s + vstart,
							vend - vstart, &pos,
							&v[n], &k[n]))
					break;
				n++;
			}
			if (n) {
				unsigned short r[4];
				unsigned char rk[4];
				int i;

				if (n == 1) {
					for (i = 0; i < 4; i++) {
						r[i] = v[0];
						rk[i] = k[0];
					}
				} else if (n == 2) {
					r[0] = r[2] = v[0];
					r[1] = r[3] = v[1];
					rk[0] = rk[2] = k[0];
					rk[1] = rk[3] = k[1];
				} else if (n == 3) {
					r[0] = v[0];
					r[1] = r[3] = v[1];
					r[2] = v[2];
					rk[0] = k[0];
					rk[1] = rk[3] = k[1];
					rk[2] = k[2];
				} else {
					for (i = 0; i < 4; i++) {
						r[i] = v[i];
						rk[i] = k[i];
					}
				}
				for (i = 0; i < 4; i++)
					r[i] = rk[i] == 2 ? 255 :
					       (r[i] > 255 ? 255 : r[i]);
				if (str_eq(prop, "border-top-left-radius")) {
					decl_add(out, nout, PROP_RADIUS_TL,
						 (short)r[0], important);
				} else if (str_eq(prop,
						  "border-top-right-radius")) {
					decl_add(out, nout, PROP_RADIUS_TR,
						 (short)r[0], important);
				} else if (str_eq(prop,
						  "border-bottom-right-radius")) {
					decl_add(out, nout, PROP_RADIUS_BR,
						 (short)r[0], important);
				} else if (str_eq(prop,
						  "border-bottom-left-radius")) {
					decl_add(out, nout, PROP_RADIUS_BL,
						 (short)r[0], important);
				} else {
					decl_add(out, nout, PROP_RADIUS_TL,
						 (short)r[0], important);
					decl_add(out, nout, PROP_RADIUS_TR,
						 (short)r[1], important);
					decl_add(out, nout, PROP_RADIUS_BR,
						 (short)r[2], important);
					decl_add(out, nout, PROP_RADIUS_BL,
						 (short)r[3], important);
				}
			}
		} else if (str_eq(prop, "border") ||
			   str_eq(prop, "border-top") ||
			   str_eq(prop, "border-right") ||
			   str_eq(prop, "border-bottom") ||
			   str_eq(prop, "border-left") ||
			   str_eq(prop, "border-color") ||
			   str_eq(prop, "border-width")) {
			unsigned short c;
			int bw;
			int have_color;

			if (vend - vstart >= 4 &&
			    ci_eq_n(s + vstart, "none", 4)) {
				decl_add(out, nout, PROP_BORDER_W, 0, important);
			} else {
				bw = parse_border_width(s + vstart,
							vend - vstart);
				have_color = find_color(s + vstart,
							vend - vstart, &c);
				if (have_color) {
					decl_add(out, nout, PROP_BORDER_COLOR, c,
						 important);
					if (bw < 0)
						bw = 1;
				}
				if (bw >= 0) {
					if (bw > 3)
						bw = 3;
					decl_add(out, nout, PROP_BORDER_W, bw,
						 important);
				}
			}
		} else if (str_eq(prop, "margin") || str_eq(prop, "padding")) {
			int vals[4], nv = 0;
			unsigned int pos = 0;
			int is_pad = prop[0] == 'p';
			unsigned char base = is_pad ? PROP_PT : PROP_MT;

			while (pos < vend - vstart && nv < 4) {
				vals[nv++] = parse_len(s + vstart,
						       vend - vstart, &pos);
				while (pos < vend - vstart &&
				       is_space(s[vstart + pos]))
					pos++;
			}
			if (nv == 1) {
				vals[1] = vals[2] = vals[3] = vals[0];
			} else if (nv == 2) {
				vals[2] = vals[0];
				vals[3] = vals[1];
			} else if (nv == 3) {
				vals[3] = vals[1];
			}
			if (nv > 0) {
				decl_add(out, nout, base, vals[0], important);
				decl_add(out, nout, (unsigned char)(base + 1),
					 vals[1], important);
				decl_add(out, nout, (unsigned char)(base + 2),
					 vals[2], important);
				decl_add(out, nout, (unsigned char)(base + 3),
					 vals[3], important);
			}
		} else if (str_eq(prop, "margin-top") ||
			   str_eq(prop, "margin-right") ||
			   str_eq(prop, "margin-bottom") ||
			   str_eq(prop, "margin-left") ||
			   str_eq(prop, "padding-top") ||
			   str_eq(prop, "padding-right") ||
			   str_eq(prop, "padding-bottom") ||
			   str_eq(prop, "padding-left")) {
			unsigned int pos = 0;
			int v = parse_len(s + vstart, vend - vstart, &pos);
			unsigned char p = PROP_MT;
			const char *side;

			if (prop[0] == 'p')
				p = PROP_PT;
			side = prop + (prop[0] == 'p' ? 8 : 7);
			if (str_eq(side, "right"))
				p = (unsigned char)(p + 1);
			else if (str_eq(side, "bottom"))
				p = (unsigned char)(p + 2);
			else if (str_eq(side, "left"))
				p = (unsigned char)(p + 3);
			decl_add(out, nout, p, v, important);
		}
	}
}

/* Skip @media/@font-face/@keyframes/@import without descending into their
 * nested blocks.  This is the mobile/base rules path: media enhancements are
 * deliberately ignored for now. */
static unsigned int skip_at_rule(const char *s, unsigned int len,
				 unsigned int i)
{
	unsigned int j = i;

	while (j < len && s[j] != '{' && s[j] != ';')
		j++;
	if (j >= len)
		return len;
	if (s[j] == ';')
		return j + 1;
	j++;
	return css_skip_nested(s, len, j);
}

static unsigned int viewport_w = 1024;
static unsigned short theme_text = 0x0000;
static unsigned short theme_bg = 0xFFFF;
static unsigned short theme_panel = 0xEF5D;
static unsigned short theme_link = 0x001B;

void br_set_theme(unsigned short text, unsigned short bg,
		  unsigned short panel, unsigned short link)
{
	theme_text = text;
	theme_bg = bg;
	theme_panel = panel;
	theme_link = link;
}


void br_set_viewport(unsigned int width)
{
	viewport_w = width ? width : 1024;
}

static unsigned int media_len_px(const char *s, unsigned int len)
{
	unsigned int i = 0, whole = 0, frac = 0, scale = 1, v;
	int em = 0;

	while (i < len && is_space(s[i]))
		i++;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		whole = whole * 10 + (unsigned int)(s[i] - '0');
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		/* Consume every fractional digit even when there are more
		 * than the three we keep: leaving the tail behind made the
		 * unit check miss "em" (74.9375em became 74px). */
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			if (scale < 1000) {
				frac = frac * 10 + (unsigned int)(s[i] - '0');
				scale *= 10;
			}
			i++;
		}
	}
	while (i < len && is_space(s[i]))
		i++;
	if (i + 2 <= len && ci_eq_n(s + i, "em", 2)) {
		em = 1;
		i += 2;
	} else if (i + 3 <= len && ci_eq_n(s + i, "rem", 3)) {
		em = 1;
		i += 3;
	} else if (i + 2 <= len && ci_eq_n(s + i, "px", 2)) {
		i += 2;
	}
	v = whole * 100 + (scale > 1 ? frac * 100 / scale : 0);
	if (em)
		return v * 16 / 100;
	return v / 100;
}

/* One parenthesised media feature: min-width/max-width and the modern
 * (width <= 600px) form.  Unknown features are treated as true. */
static int media_feature_matches(const char *s, unsigned int len)
{
	unsigned int i;

	while (len && is_space(*s)) { s++; len--; }
	while (len && is_space(s[len - 1])) len--;
	if (contains_ci_len(s, len, "prefers-color-scheme")) {
		/* There is no user-selectable colour scheme.  Ignore both
		 * variants so a page's base palette is used; sites whose base
		 * is dark (like the project docs) then stay dark instead of
		 * flipping to their light media override. */
		return 0;
	}
	if (len >= 9 && ci_eq_n(s, "min-width", 9)) {
		for (i = 9; i < len && s[i] != ':'; i++)
			;
		if (i < len)
			return viewport_w >= media_len_px(s + i + 1,
							  len - i - 1);
		return 1;
	}
	if (len >= 9 && ci_eq_n(s, "max-width", 9)) {
		for (i = 9; i < len && s[i] != ':'; i++)
			;
		if (i < len)
			return viewport_w <= media_len_px(s + i + 1,
							  len - i - 1);
		return 1;
	}
	for (i = 0; i < len; i++) {
		if (s[i] == '<' || s[i] == '>') {
			int max = s[i] == '<';
			unsigned int v = media_len_px(s + i + 1, len - i - 1);

			if (i + 1 < len && s[i + 1] == '=')
				v = media_len_px(s + i + 2, len - i - 2);
			return max ? viewport_w <= v : viewport_w >= v;
		}
	}
	return 1;
}

static int media_query_matches(const char *s, unsigned int len)
{
	unsigned int i;

	if (contains_ci_len(s, len, "print") &&
	    !contains_ci_len(s, len, "screen"))
		return 0;
	if (contains_ci_len(s, len, "not "))
		return 0;
	for (i = 0; i < len; i++) {
		if (s[i] == '(') {
			unsigned int st = ++i;

			while (i < len && s[i] != ')')
				i++;
			if (!media_feature_matches(s + st, i - st))
				return 0;
		}
	}
	return 1;
}

static int media_list_matches(const char *s, unsigned int len)
{
	unsigned int i = 0;

	while (i < len) {
		unsigned int st = i;

		while (i < len && s[i] != ',')
			i++;
		if (media_query_matches(s + st, i - st))
			return 1;
		i++;
	}
	return 0;
}

static unsigned int rule_hash(const char *s, unsigned int len)
{
	unsigned int h = 2166136261u, i;

	for (i = 0; i < len; i++) {
		char c = s[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		h ^= (unsigned char)c;
		h *= 16777619u;
	}
	return h % BR_RULE_HASH;
}

/* Index a parsed rule by the most selective component of its rightmost
 * compound selector.  br_style_for() then only visits rules that can
 * possibly match a node instead of scanning every rule. */
static void rule_index_add(struct br_doc *d, unsigned int idx,
			   const char *key, unsigned int len)
{
	unsigned int i, cpos = 0, clen = 0, ipos = 0, ilen = 0;
	unsigned short *head;

	for (i = 0; i < len; i++) {
		if (key[i] == '.' || key[i] == '#') {
			unsigned int st = i + 1, n = 0;

			while (st + n < len && is_ident(key[st + n]))
				n++;
			if (key[i] == '#') {
				ipos = st;
				ilen = n;
			} else if (n) {
				cpos = st;
				clen = n;
			}
			i = st + n - 1;
		}
	}
	if (ilen)
		head = &d->rule_bucket[rule_hash(key + ipos, ilen)];
	else if (clen)
		head = &d->rule_bucket[rule_hash(key + cpos, clen)];
	else if (len == 5 && ci_eq_n(key, ":root", 5))
		head = &d->rule_tag[BR_TAG_HTML];
	else if (len && is_ident(key[0])) {
		char name[16];
		unsigned int n = 0, tag;

		while (n < len && n + 1 < sizeof(name) && is_ident(key[n])) {
			name[n] = key[n];
			n++;
		}
		name[n] = '\0';
		tag = br_tag_from_name(name);
		if (tag != BR_TAG_UNKNOWN && tag < BR_TAG_MAX)
			head = &d->rule_tag[tag];
		else
			head = &d->rule_any;
	} else {
		head = &d->rule_any;
	}
	d->rule_next[idx] = *head;
	*head = (unsigned short)idx;
}

static unsigned int selector_specificity(const char *s, unsigned int len);

static int css_collect_only;

static void parse_css(struct br_doc *d, const char *s, unsigned int len)
{
	unsigned int i = 0;

	while (i < len && d->nrules < d->rule_cap) {
		unsigned int sstart, send, bstart, bend, j;
		struct br_decl decls[BR_DECLS];
		unsigned int nd;

		while (i < len && is_space(s[i]))
			i++;
		if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
				i++;
			i += 2;
			continue;
		}
		if (i >= len || s[i] == '}')
			break;
		if (s[i] == '@') {
			if (i + 6 <= len && ci_eq_n(s + i, "@media", 6)) {
				unsigned int q = i + 6;

				while (q < len && s[q] != '{')
					q++;
				if (q < len &&
				    media_list_matches(s + i + 6,
						       q - (i + 6))) {
					unsigned int b = q + 1;
					unsigned int e = css_skip_nested(s, len, b);

					if (e > b)
						parse_css(d, s + b,
							  (e - 1) - b);
					i = e;
				} else {
					i = skip_at_rule(s, len, i);
				}
				continue;
			}
			i = skip_at_rule(s, len, i);
			continue;
		}
		sstart = i;
		while (i < len && s[i] != '{')
			i++;
		if (i >= len)
			break;
		send = i;
		i++;
		bstart = i;
		/* CSS nesting puts @media (and &-rules) inside a rule.  Scan
		 * to the matching close brace; stopping at the first '}' left
		 * the parser out of sync and dropped the rest of the sheet
		 * (nd-aktuell lost its desktop rules that way). */
		bend = css_block_end(s, len, i);
		i = bend < len ? bend + 1 : len;
		while (sstart < send && is_space(s[sstart]))
			sstart++;
		while (send > sstart && is_space(s[send - 1]))
			send--;
		if (sstart == send)
			continue;
		if (!css_dark_selector(s + sstart, send - sstart))
			css_collect_block(d, s + bstart, bend - bstart);
		if (css_collect_only)
			continue;
		parse_decls(d, s + bstart, bend - bstart, decls, &nd);
		if (!nd)
			continue;
		/* Split a comma-separated selector list into separate rules. */
		j = sstart;
		while (j <= send && d->nrules < d->rule_cap) {
			unsigned int q = j, e, k;
			char sel[160];
			unsigned int off;

			while (q < send && s[q] != ',')
				q++;
			e = q;
			while (j < e && is_space(s[j]))
				j++;
			while (e > j && is_space(s[e - 1]))
				e--;
			if (e > j) {
				/* Ignore single selectors with sibling combinators,
				 * pseudo-classes or attribute tests: they need
				 * state/attributes we do not model.  The other
				 * selectors in the list still apply. */
				for (k = j; k < e; ) {
					if (s[k] == '+' || s[k] == '~')
						break;
					if (s[k] == ':') {
						if (k + 5 <= e &&
						    ci_eq_n(s + k, ":root", 5)) {
							k += 5;
							continue;
						}
						if (k + 5 <= e &&
						    ci_eq_n(s + k, ":not(", 5)) {
							unsigned int n = k + 5;
							int nd = 1;

							while (n < e && nd) {
								if (s[n] == '(')
									nd++;
								else if (s[n] == ')')
									nd--;
								n++;
							}
							if (nd)
								break;
							k = n;
							continue;
						}
						break;
					}
					k++;
				}
				if (k < e) {
					j = q + 1;
					if (q >= send)
						break;
					continue;
				}
				for (k = 0; k < sizeof(sel) - 1 && j + k < e; k++)
					sel[k] = s[j + k];
				sel[k] = '\0';
				off = store_attr(d, sel, k, 159);
				if (off) {
					struct br_rule *r = &d->rules[d->nrules++];
					unsigned int ks = 0, q;

					for (q = 0; q < k; q++)
						if (sel[q] == ' ' || sel[q] == '>')
							ks = q + 1;
					r->sel_off = off;
					r->key_off = store_attr(d, sel + ks,
								 k - ks, 159);
					r->spec = (unsigned short)selector_specificity(sel, k);
					r->single = 1;
					{
						unsigned int z;

						for (z = 0; z < k; z++)
							if (is_space(sel[z]) || sel[z] == '>') {
								r->single = 0;
								break;
							}
					}
					r->nd = (unsigned char)nd;
					for (k = 0; k < nd; k++)
						r->d[k] = decls[k];
					rule_index_add(d, d->nrules - 1,
						       attr_ptr(d, r->key_off),
						       s_len(attr_ptr(d, r->key_off)));
				}
			}
			j = q + 1;
			if (q >= send)
				break;
		}
	}
}

/* Sites often set html { font-size: 62.5% } and then size everything in
 * rem.  Detect that root size before parsing the sheet so a rem resolves to
 * the same pixels a real browser would use. */
static void detect_root_font(const char *s, unsigned int len)
{
	unsigned int i;

	for (i = 0; i + 4 < len; i++) {
		unsigned int j, end, sel, q;
		int v = 0, digits = 0, pct = 0, px = 0, frac = 0;
		int exact;

		if (!(s[i] == 'h' && s[i + 1] == 't' && s[i + 2] == 'm' &&
		      s[i + 3] == 'l'))
			continue;
		if (i + 4 < len && s[i + 4] != '{' && s[i + 4] != ',' &&
		    s[i + 4] != '.' && s[i + 4] != ':' && s[i + 4] != '[')
			continue;
		sel = i;
		while (sel && s[sel - 1] != '}' && s[sel - 1] != '{' &&
		       s[sel - 1] != ';' && s[sel - 1] != ',')
			sel--;
		q = sel;
		while (q < i && is_space(s[q]))
			q++;
		exact = (q == i);
		if (!exact && i + 4 < len && s[i + 4] != ',' &&
		    s[i + 4] != '.')
			continue;
		j = i + 4;
		while (j < len && is_space(s[j]))
			j++;
		if (j >= len || s[j] != '{')
			continue;
		while (j < len && s[j] != '{')
			j++;
		if (j >= len)
			break;
		end = j + 1;
		while (end < len && s[end] != '}')
			end++;
		for (; j + 9 < end; j++) {
			if (!(s[j] == 'f' && s[j + 1] == 'o' && s[j + 2] == 'n' &&
			      s[j + 3] == 't' && s[j + 4] == '-' &&
			      s[j + 5] == 's' && s[j + 6] == 'i' &&
			      s[j + 7] == 'z' && s[j + 8] == 'e'))
				continue;
			j += 9;
			while (j < end && s[j] != ':')
				j++;
			if (j >= end)
				break;
			j++;
			while (j < end && is_space(s[j]))
				j++;
			while (j < end && s[j] >= '0' && s[j] <= '9') {
				v = v * 10 + (s[j] - '0');
				digits = 1;
				j++;
			}
			if (j < end && s[j] == '.') {
				j++;
				while (j < end && s[j] >= '0' && s[j] <= '9') {
					v = v * 10 + (s[j] - '0');
					frac++;
					j++;
				}
			}
			while (frac < 2) {
				v *= 10;
				frac++;
			}
			while (frac > 2) {
				v /= 10;
				frac--;
			}
			if (!digits)
				break;
			if (j < end && s[j] == '%')
				pct = 1;
			else if (j + 1 < end && s[j] == 'p' && s[j + 1] == 'x')
				px = 1;
			if (pct)
				v = (v * 16 + 5000) / 10000;
			else if (px)
				v /= 100;
			else
				v = v * 16 / 100;
			if (exact && v >= 8 && v <= 24)
				root_font_px = v;
			if (exact)
				return;
		}
	}
}

void br_parse_css_text(struct br_doc *d, const char *css, unsigned int len)
{
	detect_root_font(css, len);
	css_collect_only = 1;
	parse_css(d, css, len);
	css_collect_only = 0;
	parse_css(d, css, len);
}

void br_parse_inline_css(struct br_doc *d)
{
	if (d->inline_css_len) {
		css_collect_only = 1;
		parse_css(d, css_store, d->inline_css_len);
		css_collect_only = 0;
		parse_css(d, css_store, d->inline_css_len);
	}
	d->inline_css_len = 0;
}

void br_reset_styles(struct br_doc *d)
{
	unsigned int i;

	for (i = 0; i < d->nnode; i++)
		d->nodes[i].style_valid = 0;
}

/* ---- selector matching ---- */

static int class_has(struct br_doc *d, unsigned int node, const char *cls,
		     unsigned int clen)
{
	const char *s = attr_ptr(d, d->nodes[node].class_off);
	unsigned int i = 0;

	while (s[i]) {
		unsigned int st, n;

		while (s[i] && is_space(s[i]))
			i++;
		st = i;
		while (s[i] && !is_space(s[i]))
			i++;
		n = i - st;
		if (n == clen && ci_eq_n(s + st, cls, n))
			return 1;
	}
	return 0;
}

static int id_eq(struct br_doc *d, unsigned int node, const char *id,
		 unsigned int len)
{
	const char *s = attr_ptr(d, d->nodes[node].id_off);
	unsigned int i;

	for (i = 0; i < len; i++)
		if (s[i] != id[i])
			return 0;
	return s[len] == '\0';
}

static int is_ident(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int attr_is(const char *name, unsigned int nlen, const char *lit)
{
	unsigned int n = 0;

	while (lit[n])
		n++;
	return n == nlen && ci_eq_n(name, lit, nlen);
}

static int attr_present(struct br_doc *d, unsigned int node,
			const char *name, unsigned int nlen)
{
	struct br_node *n = &d->nodes[node];

	if (attr_is(name, nlen, "data-collapse-target"))
		return (n->attr_flags & BR_ATTR_COLLAPSE_TARGET) != 0;
	if (attr_is(name, nlen, "data-collapse-trigger"))
		return (n->attr_flags & BR_ATTR_COLLAPSE_TRIGGER) != 0;
	if (attr_is(name, nlen, "is-open"))
		return (n->attr_flags & BR_ATTR_IS_OPEN) != 0;
	if (attr_is(name, nlen, "disabled"))
		return (n->attr_flags & BR_ATTR_DISABLED) != 0;
	if (attr_is(name, nlen, "open"))
		return (n->attr_flags & BR_ATTR_OPEN) != 0;
	if (attr_is(name, nlen, "inert"))
		return (n->attr_flags & BR_ATTR_INERT) != 0;
	if (attr_is(name, nlen, "hidden"))
		return (n->flags & BR_NODE_HIDDEN) != 0;
	if (attr_is(name, nlen, "href"))
		return attr_ptr(d, n->href_off)[0] != 0;
	if (attr_is(name, nlen, "class"))
		return attr_ptr(d, n->class_off)[0] != 0;
	if (attr_is(name, nlen, "id"))
		return attr_ptr(d, n->id_off)[0] != 0;
	if (attr_is(name, nlen, "src"))
		return attr_ptr(d, n->src_off)[0] != 0;
	if (attr_is(name, nlen, "alt"))
		return attr_ptr(d, n->alt_off)[0] != 0;
	if (attr_is(name, nlen, "rel"))
		return attr_ptr(d, n->rel_off)[0] != 0;
	return 0;
}

static int compound_match(struct br_doc *d, unsigned int node, const char *s,
			  unsigned int len)
{
	unsigned int i = 0;
	int got = 0;

	if (i < len && s[i] == '*') {
		i++;
		got = 1;
	} else if (i < len && is_ident(s[i])) {
		char name[16];
		unsigned int n = 0, tag;

		while (i < len && is_ident(s[i]) && n + 1 < sizeof(name))
			name[n++] = s[i++];
		name[n] = '\0';
		tag = br_tag_from_name(name);
		if (tag == BR_TAG_UNKNOWN) {
			const char *raw = attr_ptr(d,
						   d->nodes[node].name_off);

			if (d->nodes[node].tag != BR_TAG_UNKNOWN ||
			    !ci_eq_n(raw, name, n) || raw[n] != '\0')
				return 0;
		} else if (tag != d->nodes[node].tag) {
			return 0;
		}
		got = 1;
	}
	while (i < len) {
		if (s[i] == '.') {
			unsigned int st;
			i++;
			st = i;
			while (i < len && is_ident(s[i]))
				i++;
			if (!class_has(d, node, s + st, i - st))
				return 0;
			got = 1;
		} else if (s[i] == '#') {
			unsigned int st;
			i++;
			st = i;
			while (i < len && is_ident(s[i]))
				i++;
			if (!id_eq(d, node, s + st, i - st))
				return 0;
			got = 1;
		} else if (s[i] == '[') {
			unsigned int st, e;

			i++;
			st = e = i;
			while (e < len && s[e] != ']' && s[e] != '=' &&
			       !is_space(s[e]))
				e++;
			if (e == st || !attr_present(d, node, s + st, e - st))
				return 0;
			while (i < len && s[i] != ']')
				i++;
			if (i < len)
				i++;
			got = 1;
		} else if (s[i] == ':') {
			if (i + 5 <= len && ci_eq_n(s + i, ":root", 5) &&
			    (i + 5 == len || !is_ident(s[i + 5]))) {
				if (d->nodes[node].tag != BR_TAG_HTML)
					return 0;
				i += 5;
				got = 1;
			} else if (i + 5 <= len && ci_eq_n(s + i, ":not(", 5)) {
				unsigned int st = i + 5, e = st;
				int depth = 1;

				while (e < len && depth) {
					if (s[e] == '(')
						depth++;
					else if (s[e] == ')')
						depth--;
					if (depth)
						e++;
				}
				if (e >= len)
					return 0;
				for (unsigned int z = st; z < e; z++)
					if (s[z] == ':')
						return 0;
				if (compound_match(d, node, s + st, e - st))
					return 0;
				i = e + 1;
				got = 1;
			} else {
				return 0;  /* other pseudo-classes are not modelled */
			}
		} else {
			i++;
		}
	}
	return got;
}

static unsigned int selector_specificity(const char *s, unsigned int len)
{
	unsigned int i, spec = 0;

	for (i = 0; i < len; i++) {
		if (s[i] == '#')
			spec += 100;
		else if (s[i] == '.' || s[i] == '[' ||
			 (s[i] == ':' && !(i + 5 <= len &&
				  ci_eq_n(s + i, ":root", 5))))
			spec += 10;
		else if (i == 0 || is_space(s[i - 1]) || s[i - 1] == '>') {
			if (is_ident(s[i]))
				spec += 1;
		}
	}
	return spec;
}

/* Match one complex selector; returns specificity or -1. */
static int selector_match(struct br_doc *d, unsigned int node, const char *s,
			  unsigned int len, int right_done)
{
	struct {
		unsigned int start, end;
		int child;
	} parts[8];
	int np = 0;
	unsigned int i = 0;
	int right;

	while (i < len) {
		int child = 0;
		unsigned int st, e;

		while (i < len && is_space(s[i]))
			i++;
		if (i < len && s[i] == '>') {
			child = 1;
			i++;
			while (i < len && is_space(s[i]))
				i++;
		}
		if (i >= len)
			break;
		st = i;
		while (i < len && !is_space(s[i]) && s[i] != '>')
			i++;
		e = i;
		if (np < 8) {
			parts[np].start = st;
			parts[np].end = e;
			parts[np].child = child;
			np++;
		}
	}
	if (!np)
		return -1;
	if (!right_done &&
	    !compound_match(d, node, s + parts[np - 1].start,
			    parts[np - 1].end - parts[np - 1].start))
		return -1;
	right = np - 1;
	while (right > 0) {
		unsigned int p = d->nodes[node].parent;

		if (!p)
			return -1;
		if (parts[right].child) {
			if (!compound_match(d, p, s + parts[right - 1].start,
					    parts[right - 1].end -
					    parts[right - 1].start))
				return -1;
			node = p;
		} else {
			while (p && !compound_match(d, p,
						    s + parts[right - 1].start,
						    parts[right - 1].end -
						    parts[right - 1].start))
				p = d->nodes[p].parent;
			if (!p)
				return -1;
			node = p;
		}
		right--;
	}
	return (int)selector_specificity(s, len);
}

#define RULE_MATCH_MAX 512

struct rule_match {
	unsigned short idx;
	short spec;
};

static struct rule_match rule_matches[RULE_MATCH_MAX];
static unsigned int nrule_matches;

static void rule_try(struct br_doc *d, unsigned int node, unsigned int idx)
{
	struct br_rule *r = &d->rules[idx];
	const char *key = attr_ptr(d, r->key_off);
	int spec, key_ok = 0;

	if (key[0]) {
		if (!compound_match(d, node, key, s_len(key)))
			return;
		key_ok = 1;
	}
	spec = (r->single && key_ok) ? (int)r->spec :
		selector_match(d, node, attr_ptr(d, r->sel_off),
			      s_len(attr_ptr(d, r->sel_off)), key_ok);
	if (spec < 0)
		return;
	if (nrule_matches < RULE_MATCH_MAX) {
		rule_matches[nrule_matches].idx = (unsigned short)idx;
		rule_matches[nrule_matches].spec = (short)spec;
		nrule_matches++;
	}
}

static void rule_scan_list(struct br_doc *d, unsigned int node,
			   unsigned short head)
{
	while (head != 0xFFFF) {
		rule_try(d, node, head);
		head = d->rule_next[head];
	}
}

static void rule_scan_class(struct br_doc *d, unsigned int node,
			    const char *cls, unsigned int len)
{
	if (len)
		rule_scan_list(d, node,
			       d->rule_bucket[rule_hash(cls, len)]);
}

static void rule_collect(struct br_doc *d, unsigned int node)
{
	const char *cls = attr_ptr(d, d->nodes[node].class_off);
	const char *id = attr_ptr(d, d->nodes[node].id_off);
	unsigned int tag = d->nodes[node].tag;
	unsigned int i;

	nrule_matches = 0;
	rule_scan_list(d, node, d->rule_any);
	if (tag < BR_TAG_MAX)
		rule_scan_list(d, node, d->rule_tag[tag]);
	if (id[0])
		rule_scan_list(d, node,
			       d->rule_bucket[rule_hash(id, s_len(id))]);
	i = 0;
	while (cls[i]) {
		unsigned int st, n;

		while (cls[i] && is_space(cls[i]))
			i++;
		st = i;
		while (cls[i] && !is_space(cls[i]))
			i++;
		n = i - st;
		if (n)
			rule_scan_class(d, node, cls + st, n);
	}
	/* Rules must cascade in document order even though the index visits
	 * them in bucket order. */
	for (i = 1; i < nrule_matches; i++) {
		struct rule_match m = rule_matches[i];
		unsigned int j = i;

		while (j > 0 && rule_matches[j - 1].idx > m.idx) {
			rule_matches[j] = rule_matches[j - 1];
			j--;
		}
		rule_matches[j] = m;
	}
}

static void apply_decl(struct br_style *st, const struct br_decl *dcl)
{
	switch (dcl->prop) {
	case PROP_COLOR:
		st->color = (unsigned short)dcl->val;
		break;
	case PROP_BG:
		st->bg = (unsigned short)dcl->val;
		st->has_bg = 1;
		break;
	case PROP_FONT_PX:
		if (dcl->val < 8)
			st->font_px = 8;
		else if (dcl->val > 48)
			st->font_px = 48;
		else
			st->font_px = (unsigned char)dcl->val;
		break;
	case PROP_BOLD:
		st->bold = (unsigned char)(dcl->val != 0);
		break;
	case PROP_ITALIC:
		st->italic = (unsigned char)(dcl->val != 0);
		break;
	case PROP_MONO:
		st->mono = (unsigned char)(dcl->val != 0);
		break;
	case PROP_ALIGN:
		st->align = (unsigned char)dcl->val;
		break;
	case PROP_UNDERLINE:
		st->underline = (unsigned char)(dcl->val != 0);
		break;
	case PROP_DISPLAY:
		st->display = (unsigned char)dcl->val;
		break;
	case PROP_VIS_HIDDEN:
		st->vis_hidden = (unsigned char)(dcl->val != 0);
		break;
	case PROP_OPACITY_HIDDEN:
		st->opacity_hidden = (unsigned char)(dcl->val != 0);
		break;
	case PROP_OVERFLOW_HIDDEN:
		st->overflow_hidden = (unsigned char)(dcl->val != 0);
		break;
	case PROP_MT: st->mt = dcl->val; break;
	case PROP_MR: st->mr = dcl->val; break;
	case PROP_MB: st->mb = dcl->val; break;
	case PROP_ML: st->ml = dcl->val; break;
	case PROP_PT: st->pt = dcl->val; break;
	case PROP_PR: st->pr = dcl->val; break;
	case PROP_PB: st->pb = dcl->val; break;
	case PROP_PL: st->pl = dcl->val; break;
	case PROP_BORDER_W:
		st->border_w = (unsigned char)dcl->val;
		if (st->border_w && !st->border_color)
			st->border_color = st->color;
		break;
	case PROP_BORDER_COLOR:
		st->border_color = (unsigned short)dcl->val;
		break;
	case PROP_RADIUS_TL:
	case PROP_RADIUS_TR:
	case PROP_RADIUS_BR:
	case PROP_RADIUS_BL:
		st->radius[dcl->prop - PROP_RADIUS_TL] =
			(unsigned char)dcl->val;
		break;
	case PROP_LIST_NONE:
		st->no_bullet = (unsigned char)(dcl->val != 0);
		break;
	case PROP_WIDTH:
		st->width = (unsigned short)dcl->val;
		break;
	case PROP_WIDTH_KIND:
		st->width_kind = (unsigned char)dcl->val;
		break;
	case PROP_MAXW:
		st->max_width = (unsigned short)dcl->val;
		break;
	case PROP_MAXW_KIND:
		st->max_width_kind = (unsigned char)dcl->val;
		break;
	case PROP_HEIGHT:
		st->height = (unsigned short)dcl->val;
		break;
	case PROP_HEIGHT_KIND:
		st->height_kind = (unsigned char)dcl->val;
		break;
	case PROP_LH_PCT:
		st->line_h_pct = (unsigned char)dcl->val;
		break;
	case PROP_LH_PX:
		st->line_h_px = (unsigned short)dcl->val;
		break;
	case PROP_UPPER:
		st->uppercase = (unsigned char)(dcl->val != 0);
		break;
	case PROP_FLOAT:
		st->float_left = (unsigned char)dcl->val;
		break;
	case PROP_FLEX_GROW:
		st->flex_grow = (unsigned char)dcl->val;
		break;
	case PROP_FLEX_COL:
		st->flex_col = (unsigned char)(dcl->val != 0);
		break;
	case PROP_FLEX_WRAP:
		st->flex_wrap = (unsigned char)(dcl->val != 0);
		break;
	case PROP_GRID_NCOLS:
		st->grid_ncols = (unsigned char)dcl->val;
		break;
	case PROP_GRID_GAP:
		st->grid_gap = (unsigned char)dcl->val;
		break;
	case PROP_GRID_SLOT:
		st->grid_slot = (unsigned char)dcl->val;
		break;
	case PROP_GRID_AUTO:
		st->grid_auto = (unsigned char)(dcl->val != 0);
		break;
	case PROP_POS:
		st->pos = (unsigned char)dcl->val;
		break;
	case PROP_LEFT:
		st->left = dcl->val;
		break;
	case PROP_TOP:
		st->top = dcl->val;
		break;
	case PROP_LEFT_SET:
		st->left_set = (unsigned char)(dcl->val != 0);
		break;
	case PROP_TOP_SET:
		st->top_set = (unsigned char)(dcl->val != 0);
		break;
	default:
		break;
	}
}

static void cascade_decl(struct br_style *st, const struct br_decl *dcl,
			 int spec, short *best, short *best_imp)
{
	unsigned char p = dcl->prop;

	if (p >= PROP_COUNT)
		return;
	if (dcl->important) {
		if (spec >= best_imp[p]) {
			apply_decl(st, dcl);
			best_imp[p] = (short)spec;
		}
	} else if (best_imp[p] < 0 && spec >= best[p]) {
		apply_decl(st, dcl);
		best[p] = (short)spec;
	}
}

void br_style_for(struct br_doc *d, unsigned int node,
		  const struct br_style *parent, struct br_style *out)
{
	unsigned int tag;
	short best[PROP_COUNT], best_imp[PROP_COUNT];
	unsigned int i;

	if (d->nodes[node].style_valid) {
		*out = d->nodes[node].style;
		return;
	}

	*out = *parent;
	out->has_bg = 0;
	out->float_left = 0;
	out->border_w = 0;
	out->border_color = 0;
	for (i = 0; i < 4; i++)
		out->radius[i] = 0;
	out->width = out->max_width = out->height = 0;
	out->width_kind = out->max_width_kind = out->height_kind = 0;
	out->mt = out->mr = out->mb = out->ml = 0;
	out->pt = out->pr = out->pb = out->pl = 0;
	out->flex_grow = out->flex_col = out->flex_wrap = 0;
	out->grid_ncols = out->grid_gap = out->grid_slot = 0;
	out->grid_auto = 0;
	out->pos = out->left_set = out->top_set = 0;
	out->left = out->top = 0;
	out->vis_hidden = out->opacity_hidden = 0;
	out->overflow_hidden = 0;
	tag = d->nodes[node].tag;
	out->display = br_tag_is_block(tag) ? BR_DISPLAY_BLOCK :
					      BR_DISPLAY_INLINE;
	for (i = 0; i < PROP_COUNT; i++) {
		best[i] = -1;
		best_imp[i] = -1;
	}

	switch (tag) {
	case BR_TAG_HEAD:
	case BR_TAG_TITLE:
	case BR_TAG_META:
	case BR_TAG_LINK:
	case BR_TAG_BASE:
	case BR_TAG_SCRIPT:
	case BR_TAG_STYLE:
		out->display = BR_DISPLAY_NONE;
		break;
	case BR_TAG_BODY:
		out->mt = out->mb = out->ml = out->mr = 4;
		break;
	case BR_TAG_P:
		out->mt = out->mb = 8;
		break;
	case BR_TAG_H1:
		out->font_px = 32; out->bold = 1;
		out->mt = 10; out->mb = 6;
		break;
	case BR_TAG_H2:
		out->font_px = 24; out->bold = 1;
		out->mt = 8; out->mb = 4;
		break;
	case BR_TAG_H3: case BR_TAG_H4: case BR_TAG_H5: case BR_TAG_H6:
		out->bold = 1; out->mt = 6; out->mb = 3;
		if (tag == BR_TAG_H3)
			out->font_px = 19;
		else if (tag == BR_TAG_H4)
			out->font_px = 17;
		else if (tag == BR_TAG_H5)
			out->font_px = 15;
		else
			out->font_px = 13;
		break;
	case BR_TAG_UL: case BR_TAG_OL:
		out->mt = out->mb = 6; out->pl = 20;
		break;
	case BR_TAG_LI:
		out->mt = out->mb = 2;
		break;
	case BR_TAG_BLOCKQUOTE:
		out->mt = out->mb = 6; out->ml = out->mr = 16;
		break;
	case BR_TAG_PRE:
		out->mt = out->mb = 8; out->pl = 8;
		out->has_bg = 1; out->bg = theme_panel;
		out->mono = 1;
		break;
	case BR_TAG_CENTER:
		out->align = BR_ALIGN_CENTER;
		break;
	case BR_TAG_A:
		out->color = theme_link; out->underline = 1;
		break;
	case BR_TAG_B: case BR_TAG_STRONG:
		out->bold = 1;
		break;
	case BR_TAG_I: case BR_TAG_EM:
		out->italic = 1;
		break;
	case BR_TAG_CODE:
		out->color = 0x8010;
		out->mono = 1;
		break;
	case BR_TAG_SMALL:
		out->font_px = 13;
		break;
	case BR_TAG_BIG:
		out->font_px = 20;
		break;
	case BR_TAG_TD: case BR_TAG_TH:
		out->pr = 12;
		break;
	case BR_TAG_VIDEO:
		out->display = BR_DISPLAY_BLOCK;
		break;
	default:
		break;
	}

	/* presentational <font color>: author CSS below still wins */
	if (d->nodes[node].color_off) {
		unsigned short c;
		const char *v = attr_ptr(d, d->nodes[node].color_off);
		if (parse_color(v, s_len(v), &c))
			out->color = c;
	}

	rule_collect(d, node);
	for (i = 0; i < nrule_matches; i++) {
		struct br_rule *r = &d->rules[rule_matches[i].idx];
		int spec = rule_matches[i].spec;
		unsigned int k;

		for (k = 0; k < r->nd; k++)
			cascade_decl(out, &r->d[k], spec, best, best_imp);
	}

	if (d->nodes[node].style_off) {
		struct br_decl decls[BR_DECLS];
		unsigned int nd, k;
		const char *v = attr_ptr(d, d->nodes[node].style_off);

		parse_decls(d, v, s_len(v), decls, &nd);
		for (k = 0; k < nd; k++)
			cascade_decl(out, &decls[k], 1000, best, best_imp);
	}

	if (d->nodes[node].flags & BR_NODE_HIDDEN)
		out->display = BR_DISPLAY_NONE;
	if (out->vis_hidden || out->opacity_hidden ||
	    (out->overflow_hidden && out->height_kind == 1 &&
	     out->height == 0))
		out->display = BR_DISPLAY_NONE;

	d->nodes[node].style = *out;
	d->nodes[node].style_valid = 1;
}

/* ---- HTML parser ---- */

static void stack_recount_pre(struct br_doc *d, unsigned short *stack,
			      unsigned int sp, int *pre)
{
	unsigned int i;

	*pre = 0;
	for (i = 1; i <= sp; i++)
		if (d->nodes[stack[i]].tag == BR_TAG_PRE)
			(*pre)++;
}

static void implicit_close(struct br_doc *d, unsigned short *stack,
			   unsigned int *sp, unsigned int tag)
{
	while (*sp > 0) {
		unsigned int top = d->nodes[stack[*sp]].tag;

		if (top == BR_TAG_P && br_tag_is_block(tag)) {
			(*sp)--;
			continue;
		}
		if (top == BR_TAG_LI && tag == BR_TAG_LI) {
			(*sp)--;
			continue;
		}
		if ((top == BR_TAG_TD || top == BR_TAG_TH) &&
		    (tag == BR_TAG_TD || tag == BR_TAG_TH || tag == BR_TAG_TR)) {
			(*sp)--;
			continue;
		}
		if (top == BR_TAG_TR && tag == BR_TAG_TR) {
			(*sp)--;
			continue;
		}
		break;
	}
}

static int has_attr_token(const char *s, unsigned int len, const char *name)
{
	unsigned int i, n = s_len(name);

	for (i = 0; i + n <= len; i++) {
		if (i && !is_space(s[i - 1]) && s[i - 1] != '/')
			continue;
		if (!ci_eq_n(s + i, name, n))
			continue;
		if (i + n == len || is_space(s[i + n]) || s[i + n] == '=')
			return 1;
	}
	return 0;
}

static int stack_has_svg(const struct br_doc *d,
			 const unsigned short *stack, unsigned int sp)
{
	unsigned int k;

	for (k = 0; k <= sp && k < 64; k++)
		if (d->nodes[stack[k]].tag == BR_TAG_SVG)
			return 1;
	return 0;
}

/* Skip an element subtree by finding its closing tag.  Used for SVG
 * <title>/<desc>/<metadata> metadata, which must not enter the flow or
 * overwrite the document title. */
static unsigned int skip_element(const char *html, unsigned int len,
				 unsigned int i, const char *name,
				 unsigned int n)
{
	char close[40];
	unsigned int k = 0, p;

	close[k++] = '<';
	close[k++] = '/';
	while (k < sizeof(close) - 1 && n--)
		close[k++] = *name++;
	p = (unsigned int)find_ci(html, len, close, k, i);
	if ((int)p < 0)
		return len;
	p += k;
	while (p < len && html[p] != '>')
		p++;
	if (p < len)
		p++;
	return p;
}

int br_parse(struct br_doc *d, const char *html, unsigned int len)
{
	unsigned short stack[64];
	unsigned int sp = 0;
	unsigned int i = 0;
	int pre = 0;
	unsigned int css_len = 0;

	if (!d)
		return -1;
	if (!d->nodes)
		br_doc_init(d);
	doc_reset(d);
	stack[0] = 0;
	d->nodes[0].type = BR_NODE_ELEM;
	d->nodes[0].tag = BR_TAG_UNKNOWN;

	while (i < len) {
		if (html[i] != '<') {
			unsigned int st = i;

			while (i < len && html[i] != '<')
				i++;
			if (sp <= 63)
				append_text(d, stack[sp], html + st, i - st, pre);
			continue;
		}

		if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' &&
		    html[i + 3] == '-') {
			int p = find_ci(html, len, "-->", 3, i + 4);
			i = p >= 0 ? (unsigned int)p + 3 : len;
			continue;
		}
		if (i + 1 < len && (html[i + 1] == '!' || html[i + 1] == '?')) {
			while (i < len && html[i] != '>')
				i++;
			if (i < len)
				i++;
			continue;
		}
		if (i + 1 < len && html[i + 1] == '/') {
			unsigned int ns = i + 2, ne, tag, k, n;
			char name[32];

			while (ns < len && is_space(html[ns]))
				ns++;
			ne = ns;
			while (ne < len && is_ident(html[ne]))
				ne++;
			if (ne == ns) {
				i++;
				continue;
			}
			n = ne - ns;
			if (n >= sizeof(name))
				n = sizeof(name) - 1;
			for (k = 0; k < n; k++)
				name[k] = html[ns + k];
			name[n] = '\0';
			tag = br_tag_from_name(name);
			i = ne;
			while (i < len && html[i] != '>')
				i++;
			if (i < len)
				i++;
			for (k = sp; k > 0; k--) {
				struct br_node *nd = &d->nodes[stack[k]];

				if (tag == BR_TAG_UNKNOWN) {
					const char *raw = attr_ptr(d, nd->name_off);

					if (nd->tag == BR_TAG_UNKNOWN &&
					    ci_eq_n(raw, name, n) &&
					    raw[n] == '\0') {
						sp = k - 1;
						break;
					}
				} else if (nd->tag == tag) {
					sp = k - 1;
					break;
				}
			}
			stack_recount_pre(d, stack, sp, &pre);
			continue;
		}

		/* start tag */
		{
			unsigned int ns = i + 1, ne, tag, k, n;
			char name[16];
			unsigned int attr_start, attr_end;
			int selfclose = 0;

			while (ns < len && is_space(html[ns]))
				ns++;
			ne = ns;
			while (ne < len && (is_ident(html[ne]) || html[ne] == ':'))
				ne++;
			if (ne == ns) {
				/* stray '<' */
				append_text(d, stack[sp], html + i, 1, pre);
				i++;
				continue;
			}
			n = ne - ns;
			if (n >= sizeof(name))
				n = sizeof(name) - 1;
			for (k = 0; k < n; k++)
				name[k] = html[ns + k];
			name[n] = '\0';
			tag = br_tag_from_name(name);

			attr_start = ne;
			{
				unsigned int q = ne;
				char quote = 0;
				while (q < len) {
					char c = html[q];
					if (quote) {
						if (c == quote)
							quote = 0;
					} else if (c == '"' || c == '\'') {
						quote = c;
					} else if (c == '>') {
						break;
					}
					q++;
				}
				attr_end = q;
				if (q > attr_start && html[q - 1] == '/')
					selfclose = 1;
				i = q < len ? q + 1 : len;
			}
			if (tag == BR_TAG_SCRIPT) {
				int p = find_ci(html, len, "</script", 8, i);
				i = p >= 0 ? (unsigned int)p + 8 : len;
				while (i < len && html[i] != '>')
					i++;
				if (i < len)
					i++;
				continue;
			}
			if (stack_has_svg(d, stack, sp) &&
			    (tag == BR_TAG_TITLE ||
			     (tag == BR_TAG_UNKNOWN &&
			      (ci_eq_n(name, "desc", 4) ||
			       ci_eq_n(name, "metadata", 8))))) {
				i = skip_element(html, len, i, name, n);
				continue;
			}
			if (tag == BR_TAG_DETAILS && !selfclose &&
			    !has_attr_token(html + attr_start,
					    attr_end - attr_start, "open")) {
				/* Closed <details> is collapsed (and this browser has
				 * no script to open it); skipping the subtree keeps
				 * mobile mega-menus out of the node arena. */
				int p = find_ci(html, len, "</details>", 10, i);
				i = p >= 0 ? (unsigned int)p + 10 : len;
				continue;
			}
			if (tag == BR_TAG_STYLE) {
				int p = find_ci(html, len, "</style", 7, i);
				unsigned int end = p >= 0 ? (unsigned int)p : len;
				unsigned int cend = end;
				while (cend > i && is_space(html[cend - 1]))
					cend--;
				if (css_len + (cend - i) + 2 < BR_CSS_SIZE) {
					for (k = i; k < cend; k++)
						css_store[css_len++] = html[k];
					css_store[css_len++] = '\n';
				}
				i = p >= 0 ? (unsigned int)p + 7 : len;
				while (i < len && html[i] != '>')
					i++;
				if (i < len)
					i++;
				continue;
			}
			if (tag == BR_TAG_TITLE) {
				int p = find_ci(html, len, "</title", 7, i);
				unsigned int end = p >= 0 ? (unsigned int)p : len;
				unsigned int tn = 0;
				char tmp[BR_TITLE_MAX];

				tn = decode_text(html + i, end - i, tmp,
						 sizeof(tmp), 1);
				(void)tn;
				s_copy(d->title, tmp, sizeof(d->title));
				i = p >= 0 ? (unsigned int)p + 7 : len;
				while (i < len && html[i] != '>')
					i++;
				if (i < len)
					i++;
				continue;
			}

			if (tag == BR_TAG_BR || tag == BR_TAG_HR ||
			    tag == BR_TAG_IMG || br_tag_is_void(tag)) {
				unsigned int idx;
				implicit_close(d, stack, &sp, tag);
				idx = node_append(d, stack[sp], BR_NODE_ELEM,
						  (unsigned char)tag);
				if (idx)
					parse_attrs(d, &d->nodes[idx],
						    html + attr_start,
						    attr_end - attr_start);
				continue;
			}

			if (sp >= 63) {
				d->truncated = 1;
				continue;
			}
			implicit_close(d, stack, &sp, tag);
			{
				unsigned int idx = node_append(d, stack[sp],
							       BR_NODE_ELEM,
							       (unsigned char)tag);
				if (!idx)
					continue;
				parse_attrs(d, &d->nodes[idx],
					    html + attr_start,
					    attr_end - attr_start);
				if (tag == BR_TAG_UNKNOWN)
					d->nodes[idx].name_off =
						store_attr(d, name, n, 31);
				if (!selfclose) {
					sp++;
					stack[sp] = (unsigned short)idx;
					if (tag == BR_TAG_PRE)
						pre++;
				}
			}
		}
	}

	d->inline_css_len = css_len;
	return 0;
}

int br_parse_plain(struct br_doc *d, const char *text, unsigned int len)
{
	if (!d->nodes)
		br_doc_init(d);
	doc_reset(d);
	d->plain = 1;
	d->nodes[0].type = BR_NODE_ELEM;
	d->nodes[0].tag = BR_TAG_UNKNOWN;
	append_text(d, 0, text, len, 1);
	return 0;
}

/* A standalone image response: a synthetic document with one <img>.  The
 * caller decodes the already-fetched body into image slot 0 and sets the
 * returned node's img_index. */
unsigned int br_parse_image_doc(struct br_doc *d, const char *src)
{
	unsigned int idx;

	if (!d->nodes)
		br_doc_init(d);
	doc_reset(d);
	d->nodes[0].type = BR_NODE_ELEM;
	d->nodes[0].tag = BR_TAG_UNKNOWN;
	idx = node_append(d, 0, BR_NODE_ELEM, BR_TAG_IMG);
	if (!idx)
		return 0;
	d->nodes[idx].src_off = store_attr(d, src, s_len(src), 220);
	return idx;
}
