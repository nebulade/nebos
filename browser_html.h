/* browser_html: a small tolerant HTML tree + CSS cascade for the browser.
 *
 * Freestanding and independent of ngui: it parses a byte buffer into a static
 * node/string arena and resolves the subset of CSS the browser understands.
 * One document is live at a time (the browser is single-threaded). */
#ifndef BROWSER_HTML_H
#define BROWSER_HTML_H

#define BR_MAX_NODES     6144
#define BR_TEXT_SIZE     65536
#define BR_ATTR_SIZE     524288
#define BR_CSS_SIZE      12288
#define BR_MAX_RULES     4608
#define BR_RULE_FALLBACK 256
#define BR_MAX_VARS      512
#define BR_TAG_MAX       128
#define BR_RULE_HASH     256
#define BR_DECLS         16
#define BR_TITLE_MAX     96

/* Node types. */
#define BR_NODE_TEXT     0
#define BR_NODE_ELEM     1

/* Text-node flag: whitespace was preserved (<pre>). */
#define BR_NODE_PREFORMAT 1
/* Element flag: the HTML hidden attribute is present. */
#define BR_NODE_HIDDEN   2

/* Display values in struct br_style. */
#define BR_DISPLAY_INLINE 0
#define BR_DISPLAY_BLOCK  1
#define BR_DISPLAY_NONE   2
#define BR_DISPLAY_FLEX   3
#define BR_DISPLAY_GRID   4

/* Text alignment. */
#define BR_ALIGN_LEFT   0
#define BR_ALIGN_CENTER 1
#define BR_ALIGN_RIGHT  2

enum br_tag {
	BR_TAG_UNKNOWN = 0,
	BR_TAG_HTML, BR_TAG_HEAD, BR_TAG_BODY, BR_TAG_TITLE, BR_TAG_STYLE,
	BR_TAG_SCRIPT, BR_TAG_META, BR_TAG_LINK, BR_TAG_BASE, BR_TAG_NOSCRIPT,
	BR_TAG_P, BR_TAG_DIV, BR_TAG_SPAN, BR_TAG_A, BR_TAG_B, BR_TAG_STRONG,
	BR_TAG_I, BR_TAG_EM, BR_TAG_U, BR_TAG_CODE, BR_TAG_FONT, BR_TAG_SMALL,
	BR_TAG_BIG, BR_TAG_BR, BR_TAG_HR, BR_TAG_IMG, BR_TAG_PICTURE,
	BR_TAG_H1, BR_TAG_H2, BR_TAG_H3, BR_TAG_H4, BR_TAG_H5, BR_TAG_H6,
	BR_TAG_UL, BR_TAG_OL, BR_TAG_LI, BR_TAG_BLOCKQUOTE, BR_TAG_PRE,
	BR_TAG_CENTER, BR_TAG_TABLE, BR_TAG_THEAD, BR_TAG_TBODY, BR_TAG_TFOOT,
	BR_TAG_TR, BR_TAG_TD, BR_TAG_TH, BR_TAG_CAPTION,
	BR_TAG_FORM, BR_TAG_INPUT, BR_TAG_BUTTON, BR_TAG_LABEL, BR_TAG_SELECT,
	BR_TAG_OPTION, BR_TAG_TEXTAREA, BR_TAG_IFRAME, BR_TAG_SVG,
	BR_TAG_VIDEO, BR_TAG_AUDIO, BR_TAG_CANVAS, BR_TAG_OBJECT,
	BR_TAG_MAIN, BR_TAG_HEADER, BR_TAG_FOOTER, BR_TAG_NAV, BR_TAG_SECTION,
	BR_TAG_ARTICLE, BR_TAG_ASIDE, BR_TAG_FIGURE, BR_TAG_FIGCAPTION,
	BR_TAG_DETAILS, BR_TAG_SUMMARY, BR_TAG_DL, BR_TAG_DT, BR_TAG_DD,
	BR_TAG_ADDRESS, BR_TAG_MARK, BR_TAG_TIME, BR_TAG_ABBR, BR_TAG_CITE,
	BR_TAG_Q, BR_TAG_S, BR_TAG_STRIKE, BR_TAG_SUB, BR_TAG_SUP,
	BR_TAG_BGSOUND, BR_TAG_EMBED, BR_TAG_PARAM, BR_TAG_SOURCE,
	BR_TAG_TRACK, BR_TAG_WBR, BR_TAG_AREA, BR_TAG_COL, BR_TAG_COLGROUP
};

struct br_style {
	unsigned short color;
	unsigned short bg;
	unsigned short border_color;
	unsigned char has_bg;
	unsigned char scale;        /* 1 or 2 */
	unsigned char bold;
	unsigned char underline;
	unsigned char align;        /* BR_ALIGN_* */
	unsigned char display;      /* BR_DISPLAY_* */
	unsigned char vis_hidden, opacity_hidden, overflow_hidden;
	unsigned char float_left;   /* float: left/right */
	unsigned char flex_grow;    /* flex-grow, 0..100 */
	unsigned char flex_col;     /* flex-direction: column */
	unsigned char flex_wrap;    /* flex-wrap: wrap */
	unsigned char grid_ncols;   /* 1 or 2 supported */
	unsigned char grid_gap;     /* px */
	unsigned char grid_slot;    /* 0 auto, 1 full, 2 left, 3 right */
	unsigned char pos;          /* 0 static, 1 absolute, 2 fixed */
	unsigned char left_set, top_set;
	signed short left, top;
	unsigned char no_bullet;    /* list-style: none (inherited) */
	unsigned char uppercase;    /* text-transform: uppercase (inherited) */
	unsigned char border_w;
	unsigned char width_kind;   /* 0 none, 1 px, 2 percent */
	unsigned short width;
	unsigned char max_width_kind;
	unsigned short max_width;
	unsigned char height_kind;
	unsigned short height;
	unsigned char line_h_pct;   /* percent of font size, 0 = default */
	unsigned short line_h_px;   /* absolute px, 0 = default */
	signed short mt, mb, ml, mr;
	signed short pt, pb, pl, pr;
};

struct br_node {
	unsigned char type;
	unsigned char tag;
	unsigned char flags;
	unsigned char spare;
	unsigned short parent;
	unsigned short first;
	unsigned short last;
	unsigned short next;
	unsigned short text_off;    /* text arena offset (text nodes) */
	unsigned short text_len;
	unsigned int href_off;
	unsigned int class_off;
	unsigned int id_off;
	unsigned int style_off;
	unsigned int src_off;
	unsigned int alt_off;
	unsigned int color_off;
	unsigned int rel_off;
	unsigned short width_attr;
	unsigned short height_attr;
	unsigned char width_pct;
	unsigned char height_pct;
	short img_index;            /* -1 until the image is loaded */
	struct br_style style;      /* computed-style cache */
	unsigned char style_valid;
};

struct br_decl {
	unsigned char prop;
	unsigned char important;
	short val;
};

struct br_rule {
	unsigned int sel_off;       /* selector text in the attr arena */
	unsigned int key_off;       /* rightmost compound, for a fast prefilter */
	unsigned short spec;        /* selector specificity */
	unsigned char nd;
	unsigned char single;       /* one compound, no ancestor walk needed */
	struct br_decl d[BR_DECLS];
};

struct br_doc {
	struct br_node *nodes;
	unsigned int nnode;
	unsigned int root;
	char *text;
	unsigned int text_used;
	char *attrs;
	unsigned int attrs_used;
	struct br_rule *rules;
	unsigned int nrules;
	unsigned int rule_cap;
	char title[BR_TITLE_MAX];
	unsigned int inline_css_len;
	/* CSS custom properties, stored in the attr arena.  A pragmatic
	 * single table: the last definition wins, which matches :root. */
	unsigned int var_name[BR_MAX_VARS];
	unsigned int var_val[BR_MAX_VARS];
	unsigned short var_slot[1024];
	unsigned int nvars;
	/* Rule index: rightmost class/id/tag -> linked list of rule indices. */
	unsigned short rule_next[BR_MAX_RULES];
	unsigned short rule_bucket[BR_RULE_HASH];
	unsigned short rule_tag[BR_TAG_MAX];
	unsigned short rule_any;
	int truncated;
	int plain;                  /* text/plain: one preformatted text node */
};

void br_doc_init(struct br_doc *d);
void br_set_rule_store(struct br_doc *d, struct br_rule *buf,
		       unsigned int cap);
int  br_parse(struct br_doc *d, const char *html, unsigned int len);
int  br_parse_plain(struct br_doc *d, const char *text, unsigned int len);
unsigned int br_parse_image_doc(struct br_doc *d, const char *src);

/* External CSS is parsed by the browser after br_parse(); parse the inline
 * <style> blocks last so the cascade order matches the document. */
void br_set_viewport(unsigned int width);
void br_set_theme(unsigned short text, unsigned short bg,
		  unsigned short panel, unsigned short link);
void br_parse_css_text(struct br_doc *d, const char *css, unsigned int len);
void br_parse_inline_css(struct br_doc *d);
void br_reset_styles(struct br_doc *d);

/* A node's style: parent inherited values, UA defaults, matching rules, then
 * the inline style attribute.  Results are cached in the node until
 * br_reset_styles(). */
void br_style_for(struct br_doc *d, unsigned int node,
		  const struct br_style *parent, struct br_style *out);

int  br_tag_is_block(unsigned int tag);
int  br_tag_is_void(unsigned int tag);
const char *br_tag_name(unsigned int tag);
unsigned int br_tag_from_name(const char *name);

const char *br_href(struct br_doc *d, unsigned int node);
const char *br_src(struct br_doc *d, unsigned int node);
const char *br_alt(struct br_doc *d, unsigned int node);
const char *br_rel(struct br_doc *d, unsigned int node);
const char *br_value(struct br_doc *d, unsigned int node);
const char *br_placeholder(struct br_doc *d, unsigned int node);
const char *br_type(struct br_doc *d, unsigned int node);

#endif
