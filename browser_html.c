/* browser_html: see browser_html.h.  A deliberately tolerant parser: bad
 * nesting is repaired, unknown tags are transparent, and the CSS engine only
 * implements the properties in the browser's documented subset. */

#include "browser_html.h"

/* ---- storage (one document per process) ---- */

static struct br_node nodes_store[BR_MAX_NODES];
static char text_store[BR_TEXT_SIZE];
static char attr_store[BR_ATTR_SIZE];
static struct br_rule rule_store[BR_MAX_RULES];
static char css_store[BR_CSS_SIZE];

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

unsigned int br_tag_from_name(const char *name)
{
	unsigned int i, j;
	char lower[16];

	for (i = 0; i < 15 && name[i]; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		lower[i] = c;
	}
	lower[i] = '\0';
	for (i = 0; tag_names[i].name; i++) {
		for (j = 0; lower[j] && tag_names[i].name[j] == lower[j]; j++)
			;
		if (!lower[j] && !tag_names[i].name[j])
			return tag_names[i].tag;
	}
	return BR_TAG_UNKNOWN;
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
		emit(out, cap, n, '&');
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
static unsigned short store_attr(struct br_doc *d, const char *s,
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
		unsigned short off = (unsigned short)d->attrs_used;
		d->attrs_used += n + 1;
		return off;
	}
}

static const char *attr_ptr(struct br_doc *d, unsigned short off)
{
	return off ? d->attrs + off : "";
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

/* ---- document setup / node append ---- */

void br_doc_init(struct br_doc *d)
{
	unsigned int i;

	for (i = 0; i < sizeof(*d); i++)
		((unsigned char *)d)[i] = 0;
	d->nodes = nodes_store;
	d->text = text_store;
	d->attrs = attr_store;
	d->rules = rule_store;
	d->attrs_used = 1;      /* offset 0 = "no attribute" */
	d->attrs[0] = '\0';
	d->root = 0;
}

static void doc_reset(struct br_doc *d)
{
	unsigned int i;

	d->nnode = 0;
	d->text_used = 0;
	d->attrs_used = 1;
	d->attrs[0] = '\0';
	d->nrules = 0;
	d->title[0] = '\0';
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
		d->nodes[i].color_off = 0;
		d->nodes[i].width_attr = d->nodes[i].height_attr = 0;
		d->nodes[i].img_index = -1;
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

/* Parse attributes from `s` (between the tag name and '>'). */
static void parse_attrs(struct br_doc *d, struct br_node *n,
			const char *s, unsigned int len)
{
	unsigned int i = 0;

	while (i < len) {
		char name[16];
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
			/* boolean attribute; nothing we use needs it */
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
		if (str_eq(name, "href"))
			n->href_off = store_attr(d, s + vstart, vlen, 220);
		else if (str_eq(name, "class"))
			n->class_off = store_attr(d, s + vstart, vlen, 120);
		else if (str_eq(name, "id"))
			n->id_off = store_attr(d, s + vstart, vlen, 64);
		else if (str_eq(name, "style"))
			n->style_off = store_attr(d, s + vstart, vlen, 240);
		else if (str_eq(name, "src"))
			n->src_off = store_attr(d, s + vstart, vlen, 220);
		else if (str_eq(name, "alt"))
			n->alt_off = store_attr(d, s + vstart, vlen, 120);
		else if (str_eq(name, "color"))
			n->color_off = store_attr(d, s + vstart, vlen, 32);
		else if (str_eq(name, "width"))
			n->width_attr = (unsigned short)parse_uint(s + vstart, vlen);
		else if (str_eq(name, "height"))
			n->height_attr = (unsigned short)parse_uint(s + vstart, vlen);
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
	PROP_COLOR, PROP_BG, PROP_SCALE, PROP_BOLD, PROP_ALIGN, PROP_UNDERLINE,
	PROP_DISPLAY, PROP_MT, PROP_MR, PROP_MB, PROP_ML,
	PROP_PT, PROP_PR, PROP_PB, PROP_PL, PROP_COUNT
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

/* Parse #rgb/#rrggbb/rgb(r,g,b)/a named colour. */
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
	if (i + 3 < len && ci_eq_n(s + i, "rgb", 3)) {
		unsigned int v[3], k = 0;

		while (i < len && s[i] != '(')
			i++;
		if (i >= len)
			return 0;
		i++;
		while (k < 3 && i < len) {
			unsigned int x = 0;
			while (i < len && is_space(s[i]))
				i++;
			while (i < len && s[i] >= '0' && s[i] <= '9') {
				x = x * 10 + (unsigned int)(s[i] - '0');
				i++;
			}
			if (x > 255)
				x = 255;
			v[k++] = x;
			while (i < len && s[i] != ',' && s[i] != ')')
				i++;
			if (i < len && s[i] == ',')
				i++;
		}
		if (k == 3) {
			*out = (unsigned short)(((v[0] >> 3) << 11) |
						((v[1] >> 2) << 5) | (v[2] >> 3));
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

static int parse_len(const char *s, unsigned int len, unsigned int *pos)
{
	unsigned int i = *pos;
	int sign = 1;
	int v = 0;

	while (i < len && is_space(s[i]))
		i++;
	if (i < len && (s[i] == '-' || s[i] == '+')) {
		if (s[i] == '-')
			sign = -1;
		i++;
	}
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (s[i] - '0');
		i++;
	}
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9')
			i++;
	}
	while (i < len && (s[i] == 'p' || s[i] == 'x' || s[i] == '%' ||
			   s[i] == 'e' || s[i] == 'm' || s[i] == 'r' ||
			   s[i] == 'a' || s[i] == 'd'))
		i++;
	*pos = i;
	v *= sign;
	if (v < -32)
		v = -32;
	if (v > 96)
		v = 96;
	return v;
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
		return BR_DISPLAY_INLINE;
	if (i == 10 && ci_eq_n(s, "list-item", 10))
		return BR_DISPLAY_BLOCK;
	return BR_DISPLAY_INLINE;
}

static void parse_decls(const char *s, unsigned int len, struct br_decl *out,
			unsigned int *nout)
{
	unsigned int i = 0;

	*nout = 0;
	while (i < len && *nout < 8) {
		unsigned int pstart, pend, vstart, vend, k;
		char prop[24];

		while (i < len && (is_space(s[i]) || s[i] == ';'))
			i++;
		if (i >= len)
			break;
		pstart = i;
		while (i < len && s[i] != ':' && s[i] != ';')
			i++;
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
		for (k = 0; k < sizeof(prop) - 1 && pstart + k < pend; k++) {
			char c = s[pstart + k];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			prop[k] = c;
		}
		prop[k] = '\0';
		if (str_eq(prop, "color")) {
			unsigned short c;
			if (parse_color(s + vstart, vend - vstart, &c)) {
				out[*nout].prop = PROP_COLOR;
				out[*nout].val = (short)c;
				(*nout)++;
			}
		} else if (str_eq(prop, "background-color") ||
			   str_eq(prop, "background")) {
			unsigned short c;
			if (parse_color(s + vstart, vend - vstart, &c)) {
				out[*nout].prop = PROP_BG;
				out[*nout].val = (short)c;
				(*nout)++;
			}
		} else if (str_eq(prop, "font-size")) {
			unsigned int pos = 0;
			int v = parse_len(s + vstart, vend - vstart, &pos);
			int scale = 1;

			if (v >= 20)
				scale = 2;
			if (vend - vstart >= 5 &&
			    ci_eq_n(s + vstart, "large", 5))
				scale = 2;
			if (vend - vstart >= 6 &&
			    ci_eq_n(s + vstart, "x-large", 7))
				scale = 2;
			out[*nout].prop = PROP_SCALE;
			out[*nout].val = (short)scale;
			(*nout)++;
		} else if (str_eq(prop, "font-weight")) {
			int bold = 0;
			if ((vend - vstart >= 4 &&
			     ci_eq_n(s + vstart, "bold", 4)) ||
			    (vend - vstart >= 3 &&
			     ci_eq_n(s + vstart, "700", 3)) ||
			    (vend - vstart >= 3 &&
			     ci_eq_n(s + vstart, "800", 3)) ||
			    (vend - vstart >= 3 &&
			     ci_eq_n(s + vstart, "900", 3)))
				bold = 1;
			out[*nout].prop = PROP_BOLD;
			out[*nout].val = (short)bold;
			(*nout)++;
		} else if (str_eq(prop, "text-align")) {
			int a = BR_ALIGN_LEFT;
			if ((vend - vstart >= 6 &&
			     ci_eq_n(s + vstart, "center", 6)))
				a = BR_ALIGN_CENTER;
			else if ((vend - vstart >= 5 &&
				  ci_eq_n(s + vstart, "right", 5)))
				a = BR_ALIGN_RIGHT;
			out[*nout].prop = PROP_ALIGN;
			out[*nout].val = (short)a;
			(*nout)++;
		} else if (str_eq(prop, "text-decoration")) {
			int u = 0;
			if (vend - vstart >= 9 &&
			    ci_eq_n(s + vstart, "underline", 9))
				u = 1;
			out[*nout].prop = PROP_UNDERLINE;
			out[*nout].val = (short)u;
			(*nout)++;
		} else if (str_eq(prop, "display")) {
			out[*nout].prop = PROP_DISPLAY;
			out[*nout].val = (short)parse_display(s + vstart,
							      vend - vstart);
			(*nout)++;
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
				/* props are T, R, B, L adjacent */
				out[*nout].prop = base;
				out[*nout].val = (short)vals[0];
				(*nout)++;
				out[*nout].prop = (unsigned char)(base + 1);
				out[*nout].val = (short)vals[1];
				(*nout)++;
				out[*nout].prop = (unsigned char)(base + 2);
				out[*nout].val = (short)vals[2];
				(*nout)++;
				out[*nout].prop = (unsigned char)(base + 3);
				out[*nout].val = (short)vals[3];
				(*nout)++;
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

			if (prop[0] == 'p')
				p = PROP_PT;
			if (str_eq(prop + 7, "right"))
				p = (unsigned char)(p + 1);
			else if (str_eq(prop + 7, "bottom"))
				p = (unsigned char)(p + 2);
			else if (str_eq(prop + 7, "left"))
				p = (unsigned char)(p + 3);
			out[*nout].prop = p;
			out[*nout].val = (short)v;
			(*nout)++;
		}
	}
}

static void parse_css(struct br_doc *d, const char *s, unsigned int len)
{
	unsigned int i = 0;

	while (i < len && d->nrules < BR_MAX_RULES) {
		unsigned int sstart, send, bstart, bend, j;
		struct br_decl decls[8];
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
		sstart = i;
		while (i < len && s[i] != '{')
			i++;
		if (i >= len)
			break;
		send = i;
		i++;
		bstart = i;
		while (i < len && s[i] != '}')
			i++;
		bend = i;
		if (i < len)
			i++;
		while (sstart < send && is_space(s[sstart]))
			sstart++;
		while (send > sstart && is_space(s[send - 1]))
			send--;
		if (sstart == send)
			continue;
		if (s[sstart] == '@')
			continue;
		/* Ignore sibling combinators and pseudo-classes: they need
		 * state we do not model. */
		for (j = sstart; j < send; j++)
			if (s[j] == '+' || s[j] == '~' || s[j] == ':')
				break;
		if (j < send)
			continue;
		parse_decls(s + bstart, bend - bstart, decls, &nd);
		if (!nd)
			continue;
		/* Split a comma-separated selector list into separate rules. */
		j = sstart;
		while (j <= send && d->nrules < BR_MAX_RULES) {
			unsigned int q = j, e, k;
			char sel[80];
			unsigned int off;

			while (q < send && s[q] != ',')
				q++;
			e = q;
			while (j < e && is_space(s[j]))
				j++;
			while (e > j && is_space(s[e - 1]))
				e--;
			if (e > j) {
				for (k = 0; k < sizeof(sel) - 1 && j + k < e; k++)
					sel[k] = s[j + k];
				sel[k] = '\0';
				off = store_attr(d, sel, k, 79);
				if (off) {
					struct br_rule *r = &d->rules[d->nrules++];
					r->sel_off = (unsigned short)off;
					r->nd = (unsigned char)nd;
					for (k = 0; k < nd; k++)
						r->d[k] = decls[k];
				}
			}
			j = q + 1;
			if (q >= send)
				break;
		}
	}
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
		if (tag != d->nodes[node].tag)
			return 0;
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
		} else if (s[i] == ':') {
			return 0;      /* pseudo-classes are not modelled */
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
		else if (s[i] == '.')
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
			  unsigned int len)
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
	if (!compound_match(d, node, s + parts[np - 1].start,
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
	case PROP_SCALE:
		st->scale = (unsigned char)(dcl->val >= 2 ? 2 : 1);
		break;
	case PROP_BOLD:
		st->bold = (unsigned char)(dcl->val != 0);
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
	case PROP_MT: st->mt = dcl->val; break;
	case PROP_MR: st->mr = dcl->val; break;
	case PROP_MB: st->mb = dcl->val; break;
	case PROP_ML: st->ml = dcl->val; break;
	case PROP_PT: st->pt = dcl->val; break;
	case PROP_PR: st->pr = dcl->val; break;
	case PROP_PB: st->pb = dcl->val; break;
	case PROP_PL: st->pl = dcl->val; break;
	default: break;
	}
}

void br_style_for(struct br_doc *d, unsigned int node,
		  const struct br_style *parent, struct br_style *out)
{
	unsigned int tag;
	short best[PROP_COUNT];
	unsigned int i;

	*out = *parent;
	out->has_bg = 0;
	out->mt = out->mr = out->mb = out->ml = 0;
	out->pt = out->pr = out->pb = out->pl = 0;
	tag = d->nodes[node].tag;
	out->display = br_tag_is_block(tag) ? BR_DISPLAY_BLOCK :
					      BR_DISPLAY_INLINE;
	for (i = 0; i < PROP_COUNT; i++)
		best[i] = -1;

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
		out->scale = 2; out->bold = 1;
		out->mt = 10; out->mb = 6;
		break;
	case BR_TAG_H2:
		out->scale = 2; out->bold = 1;
		out->mt = 8; out->mb = 4;
		break;
	case BR_TAG_H3: case BR_TAG_H4: case BR_TAG_H5: case BR_TAG_H6:
		out->bold = 1; out->mt = 6; out->mb = 3;
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
		out->has_bg = 1; out->bg = 0xEF5D;
		break;
	case BR_TAG_CENTER:
		out->align = BR_ALIGN_CENTER;
		break;
	case BR_TAG_A:
		out->color = 0x001B; out->underline = 1;
		break;
	case BR_TAG_B: case BR_TAG_STRONG:
		out->bold = 1;
		break;
	case BR_TAG_CODE:
		out->color = 0x8010;
		break;
	case BR_TAG_TD: case BR_TAG_TH:
		out->pr = 12;
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

	for (i = 0; i < d->nrules; i++) {
		struct br_rule *r = &d->rules[i];
		const char *sel = attr_ptr(d, r->sel_off);
		int spec = selector_match(d, node, sel, s_len(sel));
		unsigned int k;

		if (spec < 0)
			continue;
		for (k = 0; k < r->nd; k++) {
			unsigned char p = r->d[k].prop;
			if (spec >= best[p]) {
				apply_decl(out, &r->d[k]);
				best[p] = (short)spec;
			}
		}
	}

	if (d->nodes[node].style_off) {
		struct br_decl decls[8];
		unsigned int nd, k;
		const char *v = attr_ptr(d, d->nodes[node].style_off);

		parse_decls(v, s_len(v), decls, &nd);
		for (k = 0; k < nd; k++)
			apply_decl(out, &decls[k]);
	}
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
			unsigned int ns = i + 2, ne, tag;

			while (ns < len && is_space(html[ns]))
				ns++;
			ne = ns;
			while (ne < len && is_ident(html[ne]))
				ne++;
			if (ne == ns) {
				i++;
				continue;
			}
			{
				char name[16];
				unsigned int k, n = ne - ns;
				if (n >= sizeof(name))
					n = sizeof(name) - 1;
				for (k = 0; k < n; k++)
					name[k] = html[ns + k];
				name[n] = '\0';
				tag = br_tag_from_name(name);
			}
			i = ne;
			while (i < len && html[i] != '>')
				i++;
			if (i < len)
				i++;
			{
				unsigned int k;
				for (k = sp; k > 0; k--) {
					if (d->nodes[stack[k]].tag == tag) {
						sp = k - 1;
						break;
					}
				}
				stack_recount_pre(d, stack, sp, &pre);
			}
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
				if (!selfclose) {
					sp++;
					stack[sp] = (unsigned short)idx;
					if (tag == BR_TAG_PRE)
						pre++;
				}
			}
		}
	}

	if (css_len)
		parse_css(d, css_store, css_len);
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
