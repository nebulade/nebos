/* Browser bindings for MuJS: the DOM layer (interned node wrappers,
 * attributes, query selectors, classList/dataset/style, innerHTML, timers)
 * plus document/window/location/navigator globals. */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucmd_lib.h"
#include "browser_html.h"
#include "js_browser.h"
#include "mujs.h"

/* Instructions per script/handler.  MuJS consumes the budget globally, so
 * it is reset before every entry; a runaway loop becomes a catchable
 * "script ran too long" exception instead of a frozen browser. */
#define JS_RUN_LIMIT 8000000
#define JS_TIMERS 16

struct js_timer {
	int used;
	int repeat;
	unsigned int deadline;
	unsigned int interval;
	char ref[32];
};

static struct js_timer timers[JS_TIMERS];

static js_State *J;
static struct br_doc *D;
static struct js_browser_host host;
static const char *current_url;
static jmp_buf panic_env;
static int panic_armed;

static void report_cb(js_State *state, const char *message)
{
	(void)state;
	if (host.report)
		host.report(message);
}

static void panic_cb(js_State *state)
{
	(void)state;
	if (panic_armed)
		longjmp(panic_env, 1);
	if (host.report)
		host.report("uncaught JavaScript exception");
}

static unsigned int node_of(void *p)
{
	return (unsigned int)(uintptr_t)p - 1;
}

static int node_has(js_State *state, void *p, const char *name);
static int node_put(js_State *state, void *p, const char *name);
static int node_delete(js_State *state, void *p, const char *name);
static void push_element_url(js_State *state, unsigned int node,
			     const char *name);

/* One wrapper per DOM node: the same element must have the same object
 * identity on every access, or library caches (jQuery expando data, Sizzle)
 * silently split into disconnected copies. */
static void push_node(js_State *state, unsigned int handle)
{
	char key[24];

	if (!handle) {
		js_pushnull(state);
		return;
	}
	snprintf(key, sizeof(key), "jsonnode_%u", handle);
	js_getregistry(state, key);
	if (!js_isundefined(state, -1))
		return;
	js_pop(state, 1);
	/* js_newuserdatax() pops a prototype from the top. */
	js_pushnull(state);
	js_newuserdatax(state, "node", (void *)(uintptr_t)handle,
			node_has, node_put, node_delete, 0);
	js_copy(state, -1);
	js_setregistry(state, key);
}

static unsigned int arg_node(js_State *state, int idx)
{
	if (!js_isuserdata(state, idx, "node"))
		js_typeerror(state, "not a DOM node");
	return (unsigned int)(uintptr_t)js_touserdata(state, idx, "node");
}

static void def_func_data(js_State *state, const char *name,
			  js_CFunction fn, int length, unsigned int handle)
{
	js_newcfunctionx(state, fn, name, length,
			 (void *)(uintptr_t)handle, 0);
	js_setproperty(state, -2, name);
}

static void def_func(js_State *state, const char *name,
		     js_CFunction fn, int length)
{
	js_newcfunction(state, fn, name, length);
	js_setproperty(state, -2, name);
}

static void def_global_func(js_State *state, const char *name,
			    js_CFunction fn, int length)
{
	js_newcfunction(state, fn, name, length);
	js_defglobal(state, name, JS_DONTENUM);
}

static void push_func_data(js_State *state, const char *name,
			   js_CFunction fn, int length, unsigned int handle)
{
	js_newcfunctionx(state, fn, name, length,
			 (void *)(uintptr_t)handle, 0);
}

static const char *node_attr(unsigned int node, const char *name)
{
	return br_find_attr(D, node, name);
}

static void push_attr(js_State *state, unsigned int node, const char *name,
		      int null_if_missing)
{
	const char *s = node_attr(node, name);

	if (s)
		js_pushstring(state, s);
	else if (null_if_missing)
		js_pushnull(state);
	else
		js_pushstring(state, "");
}

static unsigned int node_name(unsigned int node, char *buf, unsigned int cap)
{
	struct br_node *n;
	const char *s;
	unsigned int i = 0;

	if (!D || node >= D->nnode || cap == 0)
		return 0;
	n = &D->nodes[node];
	if (n->type == BR_NODE_TEXT)
		s = "#text";
	else if (n->type == BR_NODE_COMMENT)
		s = "#comment";
	else if (n->type == BR_NODE_FRAGMENT)
		s = "#document-fragment";
	else if (n->tag == BR_TAG_UNKNOWN && n->name_off)
		s = D->attrs + n->name_off;
	else
		s = br_tag_name(n->tag);
	while (s && *s && i + 1 < cap) {
		char c = *s++;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		buf[i++] = c;
	}
	buf[i] = '\0';
	return i;
}

static unsigned int node_type_value(unsigned int node)
{
	struct br_node *n;

	if (!D || node >= D->nnode)
		return 0;
	n = &D->nodes[node];
	if (n->type == BR_NODE_TEXT)
		return 3;
	if (n->type == BR_NODE_COMMENT)
		return 8;
	if (n->type == BR_NODE_FRAGMENT)
		return 11;
	return 1;
}

static void push_child_list(js_State *state, unsigned int node, int elements)
{
	struct br_node *n;
	unsigned int c, i = 0;

	js_newarray(state);
	if (!D || node >= D->nnode)
		return;
	n = &D->nodes[node];
	for (c = n->first; c; c = D->nodes[c].next) {
		if (elements && D->nodes[c].type != BR_NODE_ELEM)
			continue;
		push_node(state, c + 1);
		js_setindex(state, -2, (int)i++);
	}
}

static unsigned int node_prev_sibling(unsigned int node)
{
	unsigned int p, c, prev = 0;

	if (!D || node >= D->nnode)
		return 0;
	p = D->nodes[node].parent;
	for (c = D->nodes[p].first; c && c != node; c = D->nodes[c].next)
		prev = c;
	return prev;
}

/* ---- inline style and dataset objects ---- */

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

/* camelCase JS property -> CSS property ("marginTop" -> "margin-top"). */
static void css_name(const char *name, char *out, unsigned int cap)
{
	unsigned int n = 0, i;

	if (!strcmp(name, "cssFloat")) {
		if (cap > 6)
			memcpy(out, "float", 6);
		else if (cap)
			out[0] = '\0';
		return;
	}
	for (i = 0; name[i] && n + 2 < cap; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z') {
			if (n == 0 || out[n - 1] != '-')
				out[n++] = '-';
			out[n++] = (char)(c - 'A' + 'a');
		} else {
			out[n++] = c;
		}
	}
	if (cap)
		out[n] = '\0';
}

/* camelCase dataset property -> attribute name ("fooBar" -> "data-foo-bar"). */
static void data_name(const char *name, char *out, unsigned int cap)
{
	unsigned int n = 0, i;

	for (i = 0; i < 5 && n + 1 < cap; i++)
		out[n++] = "data-"[i];
	for (i = 0; name[i] && n + 2 < cap; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z') {
			out[n++] = '-';
			c = (char)(c - 'A' + 'a');
		}
		out[n++] = c;
	}
	if (cap)
		out[n] = '\0';
}

static int style_lookup(const char *style, const char *prop, char *out,
			unsigned int cap)
{
	unsigned int i = 0, nl = (unsigned int)strlen(prop);

	while (style && style[i]) {
		unsigned int ns, ne, vs, ve, a, b, k;

		while (style[i] == ' ' || style[i] == ';' || style[i] == '\t')
			i++;
		if (!style[i])
			break;
		ns = i;
		while (style[i] && style[i] != ':' && style[i] != ';')
			i++;
		ne = i;
		if (style[i] != ':') {
			if (style[i] == ';')
				i++;
			continue;
		}
		i++;
		while (style[i] == ' ' || style[i] == '\t')
			i++;
		vs = i;
		while (style[i] && style[i] != ';')
			i++;
		ve = i;
		a = ns;
		b = ne;
		while (a < b && (style[a] == ' ' || style[a] == '\t'))
			a++;
		while (b > a && (style[b - 1] == ' ' || style[b - 1] == '\t'))
			b--;
		while (ve > vs && (style[ve - 1] == ' ' || style[ve - 1] == '\t'))
			ve--;
		if (b - a == nl && ci_eq_n(style + a, prop, nl)) {
			for (k = 0; k < ve - vs && k + 1 < cap; k++)
				out[k] = style[vs + k];
			out[k < cap ? k : cap - 1] = '\0';
			return 1;
		}
	}
	return 0;
}

static unsigned int style_count(const char *style)
{
	unsigned int i = 0, n = 0;

	while (style && style[i]) {
		while (style[i] == ' ' || style[i] == ';' || style[i] == '\t')
			i++;
		if (!style[i])
			break;
		while (style[i] && style[i] != ';')
			i++;
		n++;
	}
	return n;
}

static unsigned int style_span(const char *style, unsigned int i)
{
	while (style[i] == ' ' || style[i] == ';' || style[i] == '\t')
		i++;
	return i;
}

static void style_update(unsigned int node, const char *prop,
			 const char *value, int remove)
{
	const char *old = br_find_attr(D, node, "style");
	char arr[512];
	unsigned int n = 0, i = 0, nl = (unsigned int)strlen(prop);
	int replaced = 0;

	if (remove && !old)
		return;
	while (old && old[i] && n + 2 < sizeof(arr)) {
		unsigned int start = style_span(old, i);
		unsigned int ns, ne, ve, a, b, k;

		i = start;
		if (!old[i])
			break;
		ns = i;
		while (old[i] && old[i] != ':' && old[i] != ';')
			i++;
		ne = i;
		if (old[i] == ':') {
			i++;
			while (old[i] == ' ' || old[i] == '\t')
				i++;
			while (old[i] && old[i] != ';')
				i++;
		}
		ve = i;
		a = ns;
		b = ne;
		while (a < b && (old[a] == ' ' || old[a] == '\t'))
			a++;
		while (b > a && (old[b - 1] == ' ' || old[b - 1] == '\t'))
			b--;
		if (b - a == nl && ci_eq_n(old + a, prop, nl)) {
			replaced = 1;
			if (remove || !value || !value[0])
				continue;
		}
		for (k = ns; k < ve && n + 1 < sizeof(arr); k++)
			arr[n++] = old[k];
		if (n + 1 < sizeof(arr))
			arr[n++] = ';';
	}
	if (replaced && (remove || !value || !value[0])) {
		arr[n] = '\0';
		br_set_attr(D, node, "style", arr, n);
		if (host.changed)
			host.changed();
		return;
	}
	if (value && value[0] && n + nl + strlen(value) + 3 < sizeof(arr)) {
		for (i = 0; i < nl; i++)
			arr[n++] = prop[i];
		arr[n++] = ':';
		for (i = 0; value[i]; i++)
			arr[n++] = value[i];
		arr[n++] = ';';
	}
	arr[n] = '\0';
	br_set_attr(D, node, "style", arr, n);
	if (host.changed)
		host.changed();
}

static void style_getproperty(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	char val[256];

	if (style_lookup(br_find_attr(D, node, "style"),
			 js_tostring(state, 1), val, sizeof(val)))
		js_pushstring(state, val);
	else
		js_pushstring(state, "");
}

static void style_setproperty(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));

	style_update(node, js_tostring(state, 1),
		     js_tostring(state, 2), 0);
}

static void style_removeproperty(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	const char *prop = js_tostring(state, 1);
	char old[256];
	int had = style_lookup(br_find_attr(D, node, "style"), prop,
			       old, sizeof(old));

	style_update(node, prop, "", 1);
	if (had)
		js_pushstring(state, old);
	else
		js_pushstring(state, "");
}

static int style_has(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);
	const char *style;
	char prop[64], val[256];

	if (!D || node >= D->nnode)
		return 0;
	style = br_find_attr(D, node, "style");
	if (!strcmp(name, "cssText")) {
		js_pushstring(state, style ? style : "");
		return 1;
	}
	if (!strcmp(name, "length")) {
		js_pushnumber(state, style_count(style));
		return 1;
	}
	if (!strcmp(name, "getPropertyValue")) {
		push_func_data(state, name, style_getproperty, 1, node + 1);
		return 1;
	}
	if (!strcmp(name, "setProperty")) {
		push_func_data(state, name, style_setproperty, 2, node + 1);
		return 1;
	}
	if (!strcmp(name, "removeProperty")) {
		push_func_data(state, name, style_removeproperty, 1, node + 1);
		return 1;
	}
	css_name(name, prop, sizeof(prop));
	if (style_lookup(style, prop, val, sizeof(val)))
		js_pushstring(state, val);
	else
		js_pushstring(state, "");
	return 1;
}

static int style_put(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);
	const char *value;
	char prop[64];

	if (!D || node >= D->nnode)
		return 0;
	value = js_tostring(state, -1);
	if (!strcmp(name, "cssText")) {
		br_set_attr(D, node, "style", value, (unsigned int)strlen(value));
		if (host.changed)
			host.changed();
		return 1;
	}
	css_name(name, prop, sizeof(prop));
	style_update(node, prop, value, 0);
	return 1;
}

static void push_style_object(js_State *state, unsigned int node)
{
	char key[32];

	snprintf(key, sizeof(key), "sty_%u", node + 1);
	js_getregistry(state, key);
	if (!js_isundefined(state, -1))
		return;
	js_pop(state, 1);
	js_pushnull(state);
	js_newuserdatax(state, "style", (void *)(uintptr_t)(node + 1),
			style_has, style_put, 0, 0);
	js_copy(state, -1);
	js_setregistry(state, key);
}

static int dataset_has(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);
	char attr[64];
	const char *value;

	if (!D || node >= D->nnode)
		return 0;
	data_name(name, attr, sizeof(attr));
	value = br_find_attr(D, node, attr);
	if (value)
		js_pushstring(state, value);
	else
		js_pushundefined(state);
	return 1;
}

static int dataset_put(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);
	char attr[64];
	const char *value = js_tostring(state, -1);

	if (!D || node >= D->nnode)
		return 0;
	data_name(name, attr, sizeof(attr));
	br_set_attr(D, node, attr, value, (unsigned int)strlen(value));
	if (host.changed)
		host.changed();
	return 1;
}

static int dataset_delete(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);
	char attr[64];

	(void)state;
	if (!D || node >= D->nnode)
		return 0;
	data_name(name, attr, sizeof(attr));
	br_remove_attr(D, node, attr);
	if (host.changed)
		host.changed();
	return 1;
}

static void push_dataset(js_State *state, unsigned int node)
{
	char key[32];

	snprintf(key, sizeof(key), "ds_%u", node + 1);
	js_getregistry(state, key);
	if (!js_isundefined(state, -1))
		return;
	js_pop(state, 1);
	js_pushnull(state);
	js_newuserdatax(state, "dataset", (void *)(uintptr_t)(node + 1),
			dataset_has, dataset_put, dataset_delete, 0);
	js_copy(state, -1);
	js_setregistry(state, key);
}

static void node_class_set(unsigned int node, const char *classes)
{
	br_set_attr(D, node, "class", classes,
		    (unsigned int)strlen(classes));
	if (host.changed)
		host.changed();
}

static int class_has(const char *classes, const char *name)
{
	unsigned int i = 0;

	if (!classes)
		return 0;
	while (classes[i]) {
		unsigned int start;

		while (classes[i] == ' ' || classes[i] == '\t')
			i++;
		start = i;
		while (classes[i] && classes[i] != ' ' && classes[i] != '\t')
			i++;
		if (i - start == strlen(name) &&
		    !strncmp(classes + start, name, i - start))
			return 1;
	}
	return 0;
}

static void class_add(unsigned int node, const char *name, int add)
{
	const char *old = node_attr(node, "class");
	char buf[256];
	unsigned int n = 0, i = 0;

	if (add) {
		if (class_has(old, name))
			return;
		if (old && old[0]) {
			while (old[i] && n + 1 < sizeof(buf))
				buf[n++] = old[i++];
			if (n + 1 < sizeof(buf))
				buf[n++] = ' ';
		}
		i = 0;
		while (name[i] && n + 1 < sizeof(buf))
			buf[n++] = name[i++];
	} else {
		if (!old)
			return;
		while (old[i]) {
			unsigned int start;

			while (old[i] == ' ' || old[i] == '\t')
				i++;
			start = i;
			while (old[i] && old[i] != ' ' && old[i] != '\t')
				i++;
			if (i - start == strlen(name) &&
			    !strncmp(old + start, name, i - start))
				continue;
			if (n && n + 1 < sizeof(buf))
				buf[n++] = ' ';
			{
				unsigned int k;

				for (k = start; k < i && n + 1 < sizeof(buf);
				     k++)
					buf[n++] = old[k];
			}
		}
	}
	buf[n] = '\0';
	node_class_set(node, buf);
}

static void node_classlist_add(js_State *state)
{
	class_add(node_of(js_currentfunctiondata(state)),
		  js_tostring(state, 1), 1);
}

static void node_classlist_remove(js_State *state)
{
	class_add(node_of(js_currentfunctiondata(state)),
		  js_tostring(state, 1), 0);
}

static void node_classlist_toggle(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	const char *name = js_tostring(state, 1);
	int has = class_has(node_attr(node, "class"), name);
	int want = !has;

	if (js_isdefined(state, 2))
		want = js_toboolean(state, 2);
	if (want != has)
		class_add(node, name, want);
	js_pushboolean(state, want);
}

static void node_classlist_contains(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));

	js_pushboolean(state, class_has(node_attr(node, "class"),
					js_tostring(state, 1)));
}

static unsigned int class_count(const char *classes)
{
	unsigned int i = 0, n = 0;

	if (!classes)
		return 0;
	while (classes[i]) {
		while (classes[i] == ' ' || classes[i] == '\t')
			i++;
		if (!classes[i])
			break;
		n++;
		while (classes[i] && classes[i] != ' ' && classes[i] != '\t')
			i++;
	}
	return n;
}

static void push_attributes(js_State *state, unsigned int node)
{
	struct br_node *n;
	const char *p, *end;
	unsigned int count = 0;

	js_newobject(state);
	if (!D || node >= D->nnode)
		return;
	n = &D->nodes[node];
	p = D->attrs + n->extra_off;
	end = p + n->extra_len;
	while (p + 1 < end) {
		const char *an = p;
		const char *av;

		p += strlen(p) + 1;
		if (p >= end)
			break;
		av = p;
		p += strlen(p) + 1;
		js_newobject(state);
		js_pushstring(state, an);
		js_setproperty(state, -2, "name");
		js_pushstring(state, av);
		js_setproperty(state, -2, "value");
		js_pushboolean(state, 1);
		js_setproperty(state, -2, "specified");
		/* Array-like: attributes[i] and attributes[name] both work. */
		js_copy(state, -1);
		js_setproperty(state, -3, an);
		js_setindex(state, -2, (int)count);
		count++;
	}
	js_pushnumber(state, count);
	js_setproperty(state, -2, "length");
}

static void push_classlist(js_State *state, unsigned int node)
{
	js_newobject(state);
	def_func_data(state, "add", node_classlist_add, 1, node + 1);
	def_func_data(state, "remove", node_classlist_remove, 1, node + 1);
	def_func_data(state, "toggle", node_classlist_toggle, 1, node + 1);
	def_func_data(state, "contains", node_classlist_contains, 1, node + 1);
	js_pushnumber(state, class_count(node_attr(node, "class")));
	js_setproperty(state, -2, "length");
}

static void node_getattr(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));

	push_attr(state, node, js_tostring(state, 1), 1);
}

static void node_setattr(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	const char *name = js_tostring(state, 1);
	const char *value = js_tostring(state, 2);
	char lower[64];
	unsigned int i;

	for (i = 0; name[i] && i + 1 < sizeof(lower); i++) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		lower[i] = c;
	}
	lower[i] = '\0';
	br_set_attr(D, node, lower, value, (unsigned int)strlen(value));
	if (host.changed)
		host.changed();
}

static void node_removeattr(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	const char *name = js_tostring(state, 1);
	char lower[64];
	unsigned int i;

	for (i = 0; name[i] && i + 1 < sizeof(lower); i++) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		lower[i] = c;
	}
	lower[i] = '\0';
	br_remove_attr(D, node, lower);
	if (host.changed)
		host.changed();
}

static void node_hasattr(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));

	js_pushboolean(state, node_attr(node, js_tostring(state, 1)) != 0);
}

static void node_appendchild(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int child = arg_node(state, 1);

	if (br_append_child(D, node, child) != 0) {
		js_pushnull(state);
		return;
	}
	if (host.changed)
		host.changed();
	push_node(state, child);
}

static void node_insertbefore(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int child = arg_node(state, 1);
	unsigned int before = 0;

	if (!js_isnull(state, 2) && !js_isundefined(state, 2))
		before = arg_node(state, 2);
	if (br_insert_before(D, node, child, before) != 0) {
		js_pushnull(state);
		return;
	}
	if (host.changed)
		host.changed();
	push_node(state, child);
}

static void node_removechild(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int child = arg_node(state, 1);

	if (br_remove_child(D, node, child) != 0) {
		js_pushnull(state);
		return;
	}
	if (host.changed)
		host.changed();
	push_node(state, child);
}

static void node_replacechild(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int child = arg_node(state, 1);
	unsigned int old = arg_node(state, 2);

	br_insert_before(D, node, child, old);
	br_remove_child(D, node, old);
	if (host.changed)
		host.changed();
	push_node(state, old);
}

static void node_clone(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int h = br_clone_node(D, node + 1,
				       js_toboolean(state, 1));

	push_node(state, h);
}

static void node_contains(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int other = arg_node(state, 1);
	unsigned int p = other;

	while (1) {
		if (p == node) {
			js_pushboolean(state, 1);
			return;
		}
		if (p == 0)
			break;
		p = D->nodes[p].parent;
	}
	js_pushboolean(state, 0);
}

static void node_haschildren(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));

	js_pushboolean(state, node < D->nnode && D->nodes[node].first != 0);
}

static void node_remove(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int parent = node < D->nnode ? D->nodes[node].parent : 0;

	(void)state;
	if (parent)
		br_remove_child(D, parent, node + 1);
	if (host.changed)
		host.changed();
}

static void node_matches(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));

	js_pushboolean(state, br_matches(D, node + 1,
					 js_tostring(state, 1)));
}

static void node_getelementsbytagname(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	const char *name = js_tostring(state, 1);
	unsigned int out[256];
	unsigned int n = br_query_all(D, node + 1, name, out, 256);

	js_newarray(state);
	for (unsigned int i = 0; i < n; i++) {
		push_node(state, out[i]);
		js_setindex(state, -2, (int)i);
	}
}

static void node_getelementsbyclassname(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	const char *name = js_tostring(state, 1);
	char sel[256];
	unsigned int n = 0, i = 0;
	unsigned int out[256], count;

	while (name[i] && n + 2 < sizeof(sel)) {
		while (name[i] == ' ')
			i++;
		if (!name[i])
			break;
		sel[n++] = '.';
		while (name[i] && name[i] != ' ' && n + 1 < sizeof(sel))
			sel[n++] = name[i++];
	}
	sel[n] = '\0';
	count = br_query_all(D, node + 1, sel, out, 256);
	js_newarray(state);
	for (i = 0; i < count; i++) {
		push_node(state, out[i]);
		js_setindex(state, -2, (int)i);
	}
}

static void node_queryselector(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int out[1];
	unsigned int n = br_query_all(D, node + 1, js_tostring(state, 1),
				      out, 1);

	if (n)
		push_node(state, out[0]);
	else
		js_pushnull(state);
}

static void node_queryselectorall(js_State *state)
{
	unsigned int node = node_of(js_currentfunctiondata(state));
	unsigned int out[256];
	unsigned int n = br_query_all(D, node + 1, js_tostring(state, 1),
				      out, 256);

	js_newarray(state);
	for (unsigned int i = 0; i < n; i++) {
		push_node(state, out[i]);
		js_setindex(state, -2, (int)i);
	}
}

static void node_getrect(js_State *state)
{
	js_newobject(state);
	js_pushnumber(state, 0); js_setproperty(state, -2, "top");
	js_pushnumber(state, 0); js_setproperty(state, -2, "left");
	js_pushnumber(state, 0); js_setproperty(state, -2, "right");
	js_pushnumber(state, 0); js_setproperty(state, -2, "bottom");
	js_pushnumber(state, 0); js_setproperty(state, -2, "width");
	js_pushnumber(state, 0); js_setproperty(state, -2, "height");
}

static void node_noop(js_State *state)
{
	(void)state;
}

static void node_push_text(js_State *state, unsigned int node)
{
	unsigned int cap = D->text_used + 1;
	char *buf;
	unsigned int len;

	if (cap < 32)
		cap = 32;
	buf = (char *)malloc(cap);
	if (!buf) {
		js_pushstring(state, "");
		return;
	}
	len = br_text_content(D, node, buf, cap);
	js_pushlstring(state, buf, (int)len);
	free(buf);
}

/* Userdata property hooks: every element wrapper carries node + 1. */
static int node_has(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);
	struct br_node *n;
	char buf[64];

	if (!D || node >= D->nnode)
		return 0;
	n = &D->nodes[node];

	if (!strcmp(name, "textContent") || !strcmp(name, "innerText")) {
		node_push_text(state, node);
		return 1;
	}
	if (!strcmp(name, "nodeValue")) {
		if (n->type == BR_NODE_TEXT || n->type == BR_NODE_COMMENT)
			js_pushlstring(state, D->text + n->text_off,
				       (int)n->text_len);
		else
			js_pushnull(state);
		return 1;
	}
	if (!strcmp(name, "nodeType")) {
		js_pushnumber(state, node_type_value(node));
		return 1;
	}
	if (!strcmp(name, "nodeName") || !strcmp(name, "tagName")) {
		node_name(node, buf, sizeof(buf));
		js_pushstring(state, buf);
		return 1;
	}
	if (!strcmp(name, "parentNode") || !strcmp(name, "parentElement")) {
		unsigned int p = n->parent;
		push_node(state, p ? p + 1 : 0);
		return 1;
	}
	if (!strcmp(name, "childNodes")) {
		push_child_list(state, node, 0);
		return 1;
	}
	if (!strcmp(name, "children")) {
		push_child_list(state, node, 1);
		return 1;
	}
	if (!strcmp(name, "firstChild")) {
		push_node(state, n->first ? n->first + 1 : 0);
		return 1;
	}
	if (!strcmp(name, "lastChild")) {
		push_node(state, n->last ? n->last + 1 : 0);
		return 1;
	}
	if (!strcmp(name, "firstElementChild") ||
	    !strcmp(name, "lastElementChild")) {
		unsigned int c = n->first;
		unsigned int found = 0;

		if (!strcmp(name, "lastElementChild")) {
			for (c = n->first; c; c = D->nodes[c].next)
				if (D->nodes[c].type == BR_NODE_ELEM)
					found = c;
		} else {
			while (c && D->nodes[c].type != BR_NODE_ELEM)
				c = D->nodes[c].next;
			found = c;
		}
		push_node(state, found ? found + 1 : 0);
		return 1;
	}
	if (!strcmp(name, "nextSibling") || !strcmp(name, "nextElementSibling")) {
		unsigned int c = n->next;
		if (!strcmp(name, "nextElementSibling"))
			while (c && D->nodes[c].type != BR_NODE_ELEM)
				c = D->nodes[c].next;
		push_node(state, c ? c + 1 : 0);
		return 1;
	}
	if (!strcmp(name, "previousSibling") ||
	    !strcmp(name, "previousElementSibling")) {
		unsigned int c = node_prev_sibling(node);
		if (!strcmp(name, "previousElementSibling"))
			while (c && D->nodes[c].type != BR_NODE_ELEM)
				c = node_prev_sibling(c);
		push_node(state, c ? c + 1 : 0);
		return 1;
	}
	if (!strcmp(name, "childElementCount")) {
		unsigned int c, count = 0;
		for (c = n->first; c; c = D->nodes[c].next)
			if (D->nodes[c].type == BR_NODE_ELEM)
				count++;
		js_pushnumber(state, count);
		return 1;
	}
	if (n->tag == BR_TAG_A || n->tag == BR_TAG_AREA) {
		if (!strcmp(name, "href") || !strcmp(name, "protocol") ||
		    !strcmp(name, "host") || !strcmp(name, "hostname") ||
		    !strcmp(name, "port") || !strcmp(name, "pathname") ||
		    !strcmp(name, "search") || !strcmp(name, "hash") ||
		    !strcmp(name, "origin")) {
			push_element_url(state, node, name);
			return 1;
		}
	}
	if (!strcmp(name, "id")) { push_attr(state, node, "id", 0); return 1; }
	if (!strcmp(name, "className")) {
		push_attr(state, node, "class", 0);
		return 1;
	}
	if (!strcmp(name, "href")) { push_attr(state, node, "href", 0); return 1; }
	if (!strcmp(name, "src")) { push_attr(state, node, "src", 0); return 1; }
	if (!strcmp(name, "value")) { push_attr(state, node, "value", 0); return 1; }
	if (!strcmp(name, "type")) { push_attr(state, node, "type", 0); return 1; }
	if (!strcmp(name, "name")) { push_attr(state, node, "name", 0); return 1; }
	if (!strcmp(name, "alt")) { push_attr(state, node, "alt", 0); return 1; }
	if (!strcmp(name, "title")) { push_attr(state, node, "title", 0); return 1; }
	if (!strcmp(name, "checked") || !strcmp(name, "disabled") ||
	    !strcmp(name, "selected")) {
		js_pushboolean(state, node_attr(node, name) != 0);
		return 1;
	}
	if (!strcmp(name, "style")) {
		push_style_object(state, node);
		return 1;
	}
	if (!strcmp(name, "dataset") || !strcmp(name, "domStringMap")) {
		push_dataset(state, node);
		return 1;
	}
	if (!strcmp(name, "classList")) {
		push_classlist(state, node);
		return 1;
	}
	if (!strcmp(name, "attributes")) {
		push_attributes(state, node);
		return 1;
	}
	if (!strcmp(name, "innerHTML") || !strcmp(name, "outerHTML")) {
		char key[32];
		snprintf(key, sizeof(key), "html_%u", node + 1);
		js_getregistry(state, key);
		if (js_isundefined(state, -1)) {
			js_pop(state, 1);
			js_pushstring(state, "");
		}
		return 1;
	}
	if (!strcmp(name, "ownerDocument")) {
		js_getglobal(state, "document");
		return 1;
	}
	if (!strcmp(name, "getAttribute"))
		{ push_func_data(state, "getAttribute", node_getattr, 1, node + 1); return 1; }
	if (!strcmp(name, "setAttribute"))
		{ push_func_data(state, "setAttribute", node_setattr, 2, node + 1); return 1; }
	if (!strcmp(name, "removeAttribute"))
		{ push_func_data(state, "removeAttribute", node_removeattr, 1, node + 1); return 1; }
	if (!strcmp(name, "hasAttribute"))
		{ push_func_data(state, "hasAttribute", node_hasattr, 1, node + 1); return 1; }
	if (!strcmp(name, "appendChild"))
		{ push_func_data(state, "appendChild", node_appendchild, 1, node + 1); return 1; }
	if (!strcmp(name, "insertBefore"))
		{ push_func_data(state, "insertBefore", node_insertbefore, 2, node + 1); return 1; }
	if (!strcmp(name, "removeChild"))
		{ push_func_data(state, "removeChild", node_removechild, 1, node + 1); return 1; }
	if (!strcmp(name, "replaceChild"))
		{ push_func_data(state, "replaceChild", node_replacechild, 2, node + 1); return 1; }
	if (!strcmp(name, "cloneNode"))
		{ push_func_data(state, "cloneNode", node_clone, 1, node + 1); return 1; }
	if (!strcmp(name, "contains"))
		{ push_func_data(state, "contains", node_contains, 1, node + 1); return 1; }
	if (!strcmp(name, "hasChildNodes"))
		{ push_func_data(state, "hasChildNodes", node_haschildren, 0, node + 1); return 1; }
	if (!strcmp(name, "remove"))
		{ push_func_data(state, "remove", node_remove, 0, node + 1); return 1; }
	if (!strcmp(name, "matches") || !strcmp(name, "webkitMatchesSelector") ||
	    !strcmp(name, "mozMatchesSelector") ||
	    !strcmp(name, "msMatchesSelector"))
		{ push_func_data(state, name, node_matches, 1, node + 1); return 1; }
	if (!strcmp(name, "getElementsByTagName"))
		{ push_func_data(state, "getElementsByTagName", node_getelementsbytagname, 1, node + 1); return 1; }
	if (!strcmp(name, "getElementsByClassName"))
		{ push_func_data(state, "getElementsByClassName", node_getelementsbyclassname, 1, node + 1); return 1; }
	if (!strcmp(name, "querySelector"))
		{ push_func_data(state, "querySelector", node_queryselector, 1, node + 1); return 1; }
	if (!strcmp(name, "querySelectorAll"))
		{ push_func_data(state, "querySelectorAll", node_queryselectorall, 1, node + 1); return 1; }
	if (!strcmp(name, "addEventListener"))
		{ push_func_data(state, "addEventListener", node_noop, 2, node + 1); return 1; }
	if (!strcmp(name, "removeEventListener"))
		{ push_func_data(state, "removeEventListener", node_noop, 2, node + 1); return 1; }
	if (!strcmp(name, "dispatchEvent"))
		{ push_func_data(state, "dispatchEvent", node_noop, 1, node + 1); return 1; }
	if (!strcmp(name, "getBoundingClientRect")) {
		node_getrect(state);
		return 1;
	}
	if (!strcmp(name, "getClientRects")) {
		js_newarray(state);
		return 1;
	}
	if (!strcmp(name, "offsetWidth") || !strcmp(name, "offsetHeight") ||
	    !strcmp(name, "clientWidth") || !strcmp(name, "clientHeight") ||
	    !strcmp(name, "scrollWidth") || !strcmp(name, "scrollHeight") ||
	    !strcmp(name, "scrollTop") || !strcmp(name, "scrollLeft")) {
		js_pushnumber(state, 0);
		return 1;
	}
	if (!strcmp(name, "click") || !strcmp(name, "focus") ||
	    !strcmp(name, "blur")) {
		push_func_data(state, name, node_noop, 0, node + 1);
		return 1;
	}
	return 0;
}

static void node_set_string(unsigned int node, const char *name,
			    const char *value)
{
	br_set_attr(D, node, name, value, (unsigned int)strlen(value));
	if (host.changed)
		host.changed();
}

static int node_put(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);

	if (!D || node >= D->nnode)
		return 0;
	if (!strcmp(name, "textContent") || !strcmp(name, "innerText")) {
		const char *text = js_tostring(state, -1);
		unsigned int len = (unsigned int)strlen(text);

		if (br_set_text_content(D, node, text, len) != 0) {
			if (host.status)
				host.status("Page text arena is full", 1);
		} else if (host.changed) {
			host.changed();
		}
		return 1;
	}
	if (!strcmp(name, "nodeValue")) {
		const char *text = js_tostring(state, -1);

		if (br_set_text_node(D, node, text,
				     (unsigned int)strlen(text)) == 0 &&
		    host.changed)
			host.changed();
		return 1;
	}
	if (!strcmp(name, "id") || !strcmp(name, "className") ||
	    !strcmp(name, "href") || !strcmp(name, "src") ||
	    !strcmp(name, "value") || !strcmp(name, "type") ||
	    !strcmp(name, "name") || !strcmp(name, "alt") ||
	    !strcmp(name, "title")) {
		const char *value = js_tostring(state, -1);
		if (!strcmp(name, "className"))
			node_set_string(node, "class", value);
		else
			node_set_string(node, name, value);
		return 1;
	}
	if (!strcmp(name, "checked") || !strcmp(name, "disabled") ||
	    !strcmp(name, "selected")) {
		if (js_toboolean(state, -1))
			br_set_attr(D, node, name, "", 0);
		else
			br_remove_attr(D, node, name);
		if (host.changed)
			host.changed();
		return 1;
	}
	if (!strcmp(name, "innerHTML")) {
		const char *html = js_tostring(state, -1);
		char key[32];

		br_set_inner_html(D, node, html, (unsigned int)strlen(html));
		snprintf(key, sizeof(key), "html_%u", node + 1);
		js_pushstring(state, html);
		js_setregistry(state, key);
		if (host.changed)
			host.changed();
		return 1;
	}
	if (!strcmp(name, "outerHTML")) {
		char key[32];
		snprintf(key, sizeof(key), "html_%u", node + 1);
		js_pushstring(state, js_tostring(state, -1));
		js_setregistry(state, key);
		return 1;
	}
	if (!strcmp(name, "style")) {
		if (js_isstring(state, -1))
			node_set_string(node, "style",
					js_tostring(state, -1));
		return 1;
	}
	return 0;
}

static int node_delete(js_State *state, void *p, const char *name)
{
	(void)state;
	(void)p;
	(void)name;
	return 0;
}

static void doc_getelementbyid(js_State *state)
{
	const char *id = js_tostring(state, 1);

	push_node(state, br_find_id(D, id));
}

static void console_log(js_State *state)
{
	int i;
	int n = js_gettop(state);

	for (i = 1; i < n; i++) {
		if (i > 1)
			u_putc(' ');
		u_puts(js_tostring(state, i));
	}
	u_putc('\n');
}

static void js_alert(js_State *state)
{
	char msg[160];
	unsigned int used = 0;
	int i;
	int n = js_gettop(state);

	for (i = 1; i < n; i++) {
		const char *s = js_tostring(state, i);
		unsigned int k;

		if (i > 1 && used + 1 < sizeof(msg))
			msg[used++] = ' ';
		for (k = 0; s[k] && used + 1 < sizeof(msg); k++)
			msg[used++] = s[k];
	}
	msg[used] = '\0';
	if (host.status)
		host.status(msg, 0);
}

static void push_query_array(js_State *state, unsigned int root,
			     const char *selector)
{
	unsigned int out[256];
	unsigned int n = br_query_all(D, root, selector, out, 256);
	unsigned int i;

	js_newarray(state);
	for (i = 0; i < n; i++) {
		push_node(state, out[i]);
		js_setindex(state, -2, (int)i);
	}
}

static void doc_create_element(js_State *state)
{
	const char *name = js_tostring(state, 1);
	char lower[32];
	unsigned int i;

	for (i = 0; name[i] && i + 1 < sizeof(lower); i++) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		lower[i] = c;
	}
	lower[i] = '\0';
	push_node(state, br_create_element(D, lower, i));
}

static void doc_create_text(js_State *state)
{
	const char *text = js_tostring(state, 1);
	push_node(state, br_create_text(D, text, (unsigned int)strlen(text)));
}

static void doc_create_comment(js_State *state)
{
	const char *text = js_tostring(state, 1);
	push_node(state, br_create_comment(D, text, (unsigned int)strlen(text)));
}

static void doc_create_fragment(js_State *state)
{
	push_node(state, br_create_fragment(D));
}

static void doc_create_attribute(js_State *state)
{
	const char *name = js_tostring(state, 1);

	js_newobject(state);
	js_pushstring(state, name);
	js_setproperty(state, -2, "name");
	js_pushstring(state, "");
	js_setproperty(state, -2, "value");
	js_pushboolean(state, 1);
	js_setproperty(state, -2, "specified");
}

static void doc_getelementsbytagname(js_State *state)
{
	push_query_array(state, 0, js_tostring(state, 1));
}

static void doc_getelementsbyclassname(js_State *state)
{
	const char *name = js_tostring(state, 1);
	char sel[256];
	unsigned int n = 0, i = 0;

	while (name[i] && n + 2 < sizeof(sel)) {
		while (name[i] == ' ')
			i++;
		if (!name[i])
			break;
		sel[n++] = '.';
		while (name[i] && name[i] != ' ' && n + 1 < sizeof(sel))
			sel[n++] = name[i++];
	}
	sel[n] = '\0';
	push_query_array(state, 0, sel);
}

static void doc_queryselector(js_State *state)
{
	unsigned int out[1];
	unsigned int n = br_query_all(D, 0, js_tostring(state, 1), out, 1);

	if (n)
		push_node(state, out[0]);
	else
		js_pushnull(state);
}

static void doc_queryselectorall(js_State *state)
{
	push_query_array(state, 0, js_tostring(state, 1));
}

static void doc_noop(js_State *state)
{
	(void)state;
}

static void location_get(js_State *state)
{
	js_pushstring(state, current_url ? current_url : "");
}

static void location_set(js_State *state)
{
	const char *url = js_tostring(state, 1);

	if (host.navigate)
		host.navigate(url);
}

static void win_noop(js_State *state);

struct loc_parts {
	const char *protocol, *host, *hostname, *port, *path, *search, *hash;
	const char *origin;
	unsigned int protocol_len, host_len, hostname_len, port_len;
	unsigned int path_len, search_len, hash_len, origin_len;
};

static void loc_split(const char *u, struct loc_parts *p)
{
	unsigned int n = (unsigned int)strlen(u), i;
	const char *rest;

	memset(p, 0, sizeof(*p));
	p->path = u;
	p->path_len = n;
	p->origin = "null";
	p->origin_len = 4;

	for (i = 0; i < n; i++)
		if (u[i] == ':')
			break;
	if (i + 2 < n && u[i] == ':' && u[i + 1] == '/' && u[i + 2] == '/') {
		const char *hosts = u + i + 3, *hoste = hosts;

		while (*hoste && *hoste != '/' && *hoste != '?' && *hoste != '#')
			hoste++;
		p->protocol = u;
		p->protocol_len = i + 1;
		p->host = hosts;
		p->host_len = (unsigned int)(hoste - hosts);
		p->hostname = hosts;
		p->hostname_len = p->host_len;
		for (i = 0; i < p->hostname_len; i++)
			if (hosts[i] == ':') {
				p->port = hosts + i + 1;
				p->port_len = p->hostname_len - i - 1;
				p->hostname_len = i;
				break;
			}
		p->origin = u;
		p->origin_len = (unsigned int)(hoste - u);
		rest = hoste;
	} else {
		p->protocol = "file:";
		p->protocol_len = 5;
		rest = u;
	}

	{
		const char *q = rest;

		while (*q && *q != '?' && *q != '#')
			q++;
		p->path = rest;
		p->path_len = (unsigned int)(q - rest);
		if (*q == '?') {
			const char *s = q;

			while (*q && *q != '#')
				q++;
			p->search = s;
			p->search_len = (unsigned int)(q - s);
		}
		if (*q == '#') {
			p->hash = q;
			p->hash_len = (unsigned int)strlen(q);
		}
	}
}

static void push_location(js_State *state)
{
	struct loc_parts p;

	loc_split(current_url ? current_url : "", &p);
	js_newobject(state);
	js_newcfunction(state, location_get, "get href", 0);
	js_newcfunction(state, location_set, "set href", 1);
	js_defaccessor(state, -3, "href", 0);

#define LOC_FIELD(field) \
	do { \
		js_pushlstring(state, p.field ? p.field : "", \
			       p.field ? (int)p.field##_len : 0); \
		js_setproperty(state, -2, #field); \
	} while (0)
	LOC_FIELD(protocol);
	LOC_FIELD(host);
	LOC_FIELD(hostname);
	LOC_FIELD(port);
	js_pushlstring(state, p.path ? p.path : "",
		       p.path ? (int)p.path_len : 0);
	js_setproperty(state, -2, "pathname");
	LOC_FIELD(search);
	LOC_FIELD(hash);
	LOC_FIELD(origin);
#undef LOC_FIELD

	js_newcfunction(state, win_noop, "reload", 0);
	js_setproperty(state, -2, "reload");
	js_newcfunction(state, win_noop, "assign", 1);
	js_setproperty(state, -2, "assign");
	js_newcfunction(state, win_noop, "replace", 1);
	js_setproperty(state, -2, "replace");
	js_newcfunction(state, win_noop, "toString", 0);
	js_setproperty(state, -2, "toString");
}

/* ---- URL resolution for <a>/<area> URL properties ---- */

static int url_has_scheme(const char *s)
{
	unsigned int i = 1;

	if (!((s[0] >= 'a' && s[0] <= 'z') ||
	      (s[0] >= 'A' && s[0] <= 'Z')))
		return 0;
	while (s[i]) {
		char c = s[i];

		if (c == ':')
			return 1;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '+' || c == '-' ||
		      c == '.'))
			return 0;
		i++;
	}
	return 0;
}

static void url_put(char *out, unsigned int cap, unsigned int *n,
		    const char *s, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len && *n + 1 < cap; i++)
		out[(*n)++] = s[i];
}

/* Collapse //, /./ and /../ inside the path part of an absolute URL. */
static void url_normalize(char *url)
{
	char tmp[512];
	char *path;
	unsigned int n = 0, i = 0;

	path = strstr(url, "://");
	if (path) {
		path += 3;
		while (*path && *path != '/')
			path++;
	} else {
		path = url;
	}
	if (path[0] != '/')
		return;
	while (path[i]) {
		unsigned int start;

		while (path[i] == '/')
			i++;
		start = i;
		while (path[i] && path[i] != '/')
			i++;
		if (i == start)
			continue;
		if (i - start == 1 && path[start] == '.')
			continue;
		if (i - start == 2 && path[start] == '.' &&
		    path[start + 1] == '.') {
			while (n > 0 && tmp[n - 1] != '/')
				n--;
			if (n > 0)
				n--;
			continue;
		}
		if (n + 1 < sizeof(tmp))
			tmp[n++] = '/';
		while (start < i && n + 1 < sizeof(tmp))
			tmp[n++] = path[start++];
	}
	if (n == 0)
		tmp[n++] = '/';
	tmp[n] = '\0';
	memcpy(path, tmp, n + 1);
}

static void url_resolve(const char *base, const char *rel,
			char *out, unsigned int cap)
{
	struct loc_parts bp;
	unsigned int n = 0, i;

	if (cap == 0)
		return;
	if (!rel[0]) {
		url_put(out, cap, &n, base, (unsigned int)strlen(base));
		out[n] = '\0';
		return;
	}
	if (url_has_scheme(rel)) {
		url_put(out, cap, &n, rel, (unsigned int)strlen(rel));
		out[n] = '\0';
		url_normalize(out);
		return;
	}

	loc_split(base, &bp);

	if (rel[0] == '/' && rel[1] == '/') {
		url_put(out, cap, &n, bp.protocol, bp.protocol_len);
		url_put(out, cap, &n, rel, (unsigned int)strlen(rel));
	} else if (rel[0] == '/') {
		url_put(out, cap, &n, bp.protocol, bp.protocol_len);
		url_put(out, cap, &n, "//", 2);
		url_put(out, cap, &n, bp.host, bp.host_len);
		url_put(out, cap, &n, rel, (unsigned int)strlen(rel));
	} else if (rel[0] == '#' || rel[0] == '?') {
		url_put(out, cap, &n, bp.protocol, bp.protocol_len);
		if (bp.host_len || (bp.protocol_len == 5 &&
				    bp.protocol[0] == 'f')) {
			url_put(out, cap, &n, "//", 2);
			url_put(out, cap, &n, bp.host, bp.host_len);
		}
		url_put(out, cap, &n, bp.path, bp.path_len);
		if (rel[0] == '#')
			url_put(out, cap, &n, bp.search, bp.search_len);
		url_put(out, cap, &n, rel, (unsigned int)strlen(rel));
	} else {
		const char *dir_end = bp.path;

		for (i = 0; i < bp.path_len; i++)
			if (bp.path[i] == '/')
				dir_end = bp.path + i + 1;
		if (dir_end == bp.path)
			dir_end = bp.path + bp.path_len;
		url_put(out, cap, &n, bp.protocol, bp.protocol_len);
		if (bp.host_len || (bp.protocol_len == 5 &&
				    bp.protocol[0] == 'f')) {
			url_put(out, cap, &n, "//", 2);
			url_put(out, cap, &n, bp.host, bp.host_len);
		}
		url_put(out, cap, &n, bp.path,
			(unsigned int)(dir_end - bp.path));
		url_put(out, cap, &n, rel, (unsigned int)strlen(rel));
	}
	out[n] = '\0';
	url_normalize(out);
}

/* <a> URL properties are resolved against the page URL, like the DOM. */
static void push_element_url(js_State *state, unsigned int node,
			     const char *name)
{
	char abs_url[512];
	const char *href = br_find_attr(D, node, "href");
	struct loc_parts p;

	if (!href || !href[0]) {
		js_pushstring(state, "");
		return;
	}
	url_resolve(current_url ? current_url : "/", href,
		    abs_url, sizeof(abs_url));
	loc_split(abs_url, &p);
	if (!strcmp(name, "href"))
		js_pushstring(state, abs_url);
	else if (!strcmp(name, "protocol"))
		js_pushlstring(state, p.protocol, (int)p.protocol_len);
	else if (!strcmp(name, "origin"))
		js_pushlstring(state, p.origin ? p.origin : "",
			       (int)p.origin_len);
	else if (!strcmp(name, "host"))
		js_pushlstring(state, p.host ? p.host : "", (int)p.host_len);
	else if (!strcmp(name, "hostname"))
		js_pushlstring(state, p.hostname ? p.hostname : "",
			       (int)p.hostname_len);
	else if (!strcmp(name, "port"))
		js_pushlstring(state, p.port ? p.port : "", (int)p.port_len);
	else if (!strcmp(name, "pathname"))
		js_pushlstring(state, p.path ? p.path : "", (int)p.path_len);
	else if (!strcmp(name, "search"))
		js_pushlstring(state, p.search ? p.search : "",
			       (int)p.search_len);
	else
		js_pushlstring(state, p.hash ? p.hash : "", (int)p.hash_len);
}

static void win_noop(js_State *state)
{
	(void)state;
}

static void timer_add(js_State *state, int repeat)
{
	int i;
	unsigned int ms;

	if (!js_iscallable(state, 1)) {
		js_pushnumber(state, 0);
		return;
	}
	ms = js_isdefined(state, 2) ?
	     (unsigned int)js_tointeger(state, 2) : 0;
	if (repeat && ms == 0)
		ms = 1;
	for (i = 0; i < JS_TIMERS; i++)
		if (!timers[i].used)
			break;
	if (i == JS_TIMERS) {
		js_pushnumber(state, 0);
		return;
	}
	js_copy(state, 1);
	snprintf(timers[i].ref, sizeof(timers[i].ref), "%s",
		 js_ref(state));
	timers[i].used = 1;
	timers[i].repeat = repeat;
	timers[i].interval = ms;
	timers[i].deadline = u_ticks_ms() + ms;
	js_pushnumber(state, i + 1);
}

static void win_settimeout(js_State *state)
{
	timer_add(state, 0);
}

static void win_setinterval(js_State *state)
{
	timer_add(state, 1);
}

static void timer_clear(js_State *state)
{
	int id = js_tointeger(state, 1);

	if (id >= 1 && id <= JS_TIMERS && timers[id - 1].used) {
		js_unref(state, timers[id - 1].ref);
		timers[id - 1].used = 0;
	}
}

static void win_requestanimationframe(js_State *state)
{
	timer_add(state, 0);
}

static void win_performance_now(js_State *state)
{
	js_pushnumber(state, (double)u_ticks_ms());
}

static void win_getproperty(js_State *state)
{
	(void)state;
	js_pushstring(state, "");
}

static void win_getcomputedstyle(js_State *state)
{
	(void)state;
	js_newobject(state);
	def_func(state, "getPropertyValue", win_getproperty, 1);
	js_pushstring(state, "");
	js_setproperty(state, -2, "cssText");
}

static void win_matchmedia(js_State *state)
{
	const char *query = js_tostring(state, 1);

	js_newobject(state);
	js_pushboolean(state, 0);
	js_setproperty(state, -2, "matches");
	js_pushstring(state, query);
	js_setproperty(state, -2, "media");
	def_func(state, "addListener", win_noop, 1);
	def_func(state, "removeListener", win_noop, 1);
	def_func(state, "addEventListener", win_noop, 2);
	def_func(state, "removeEventListener", win_noop, 2);
}

static void install_globals(js_State *state)
{
	unsigned int h;

	js_newobject(state);
	def_func(state, "getElementById", doc_getelementbyid, 1);
	def_func(state, "createElement", doc_create_element, 1);
	def_func(state, "createTextNode", doc_create_text, 1);
	def_func(state, "createComment", doc_create_comment, 1);
	def_func(state, "createDocumentFragment", doc_create_fragment, 0);
	def_func(state, "createAttribute", doc_create_attribute, 1);
	def_func(state, "getElementsByTagName", doc_getelementsbytagname, 1);
	def_func(state, "getElementsByClassName", doc_getelementsbyclassname, 1);
	def_func(state, "querySelector", doc_queryselector, 1);
	def_func(state, "querySelectorAll", doc_queryselectorall, 1);
	def_func(state, "addEventListener", doc_noop, 2);
	def_func(state, "removeEventListener", doc_noop, 2);

	h = br_find_tag(D, BR_TAG_HTML);
	if (!h && D->nnode > 1)
		h = D->nodes[0].first ? D->nodes[0].first + 1 : 0;
	push_node(state, h);
	js_setproperty(state, -2, "documentElement");
	h = br_find_tag(D, BR_TAG_BODY);
	push_node(state, h);
	js_setproperty(state, -2, "body");
	h = br_find_tag(D, BR_TAG_HEAD);
	push_node(state, h);
	js_setproperty(state, -2, "head");
	js_pushnumber(state, 9);
	js_setproperty(state, -2, "nodeType");
	js_pushstring(state, "#document");
	js_setproperty(state, -2, "nodeName");
	js_pushnull(state);
	js_setproperty(state, -2, "ownerDocument");
	js_pushstring(state, "CSS1Compat");
	js_setproperty(state, -2, "compatMode");
	js_pushstring(state, D->title);
	js_setproperty(state, -2, "title");
	js_pushstring(state, "complete");
	js_setproperty(state, -2, "readyState");
	js_pushstring(state, "");
	js_setproperty(state, -2, "cookie");
	js_pushstring(state, current_url ? current_url : "");
	js_setproperty(state, -2, "URL");
	js_defglobal(state, "document", JS_DONTENUM);

	js_newobject(state);
	js_newcfunction(state, console_log, "log", 0);
	js_setproperty(state, -2, "log");
	js_defglobal(state, "console", JS_DONTENUM);

	js_newcfunction(state, js_alert, "alert", 1);
	js_defglobal(state, "alert", JS_DONTENUM);

	push_location(state);
	js_defglobal(state, "location", JS_DONTENUM);

	js_newobject(state);
	js_pushstring(state, "Mozilla/5.0 (X11; Linux i686) NebOS/0.2");
	js_setproperty(state, -2, "userAgent");
	js_pushstring(state, "NebOS");
	js_setproperty(state, -2, "platform");
	js_pushstring(state, "en-US");
	js_setproperty(state, -2, "language");
	js_defglobal(state, "navigator", JS_DONTENUM);

	js_newobject(state);
	js_pushnumber(state, 1);
	js_setproperty(state, -2, "length");
	def_func(state, "pushState", win_noop, 3);
	def_func(state, "replaceState", win_noop, 3);
	def_func(state, "back", win_noop, 0);
	def_func(state, "forward", win_noop, 0);
	def_func(state, "go", win_noop, 1);
	js_defglobal(state, "history", JS_DONTENUM);

	def_global_func(state, "matchMedia", win_matchmedia, 1);
	def_global_func(state, "getComputedStyle", win_getcomputedstyle, 1);
	def_global_func(state, "addEventListener", win_noop, 2);
	def_global_func(state, "removeEventListener", win_noop, 2);
	def_global_func(state, "scrollTo", win_noop, 2);
	def_global_func(state, "scrollBy", win_noop, 2);
	def_global_func(state, "setTimeout", win_settimeout, 1);
	def_global_func(state, "setInterval", win_setinterval, 1);
	def_global_func(state, "clearTimeout", timer_clear, 1);
	def_global_func(state, "clearInterval", timer_clear, 1);
	def_global_func(state, "requestAnimationFrame", win_requestanimationframe, 1);
	def_global_func(state, "cancelAnimationFrame", timer_clear, 1);

	js_newobject(state);
	def_func(state, "now", win_performance_now, 0);
	js_newobject(state);
	js_setproperty(state, -2, "timing");
	js_defglobal(state, "performance", JS_DONTENUM);

	js_pushnumber(state, host.width ? (int)host.width : 1024);
	js_defglobal(state, "innerWidth", JS_DONTENUM);
	js_pushnumber(state, host.height ? (int)host.height : 768);
	js_defglobal(state, "innerHeight", JS_DONTENUM);
	js_pushnumber(state, 0);
	js_defglobal(state, "pageXOffset", JS_DONTENUM);
	js_pushnumber(state, 0);
	js_defglobal(state, "pageYOffset", JS_DONTENUM);
	js_pushnumber(state, 0);
	js_defglobal(state, "scrollX", JS_DONTENUM);
	js_pushnumber(state, 0);
	js_defglobal(state, "scrollY", JS_DONTENUM);
	js_pushnumber(state, 1);
	js_defglobal(state, "devicePixelRatio", JS_DONTENUM);

	js_pushglobal(state);
	js_defglobal(state, "window", JS_DONTENUM);
	js_pushglobal(state);
	js_defglobal(state, "globalThis", JS_DONTENUM);

	js_getglobal(state, "document");
	js_getglobal(state, "location");
	js_setproperty(state, -2, "location");
	js_pop(state, 1);
	js_getglobal(state, "document");
	js_pushglobal(state);
	js_setproperty(state, -2, "defaultView");
	js_pop(state, 1);
}

int js_browser_start(struct br_doc *doc, const struct js_browser_host *h)
{
	if (!doc || !h)
		return -1;
	D = doc;
	host = *h;
	current_url = h->page_url;
	panic_armed = 0;
	memset(timers, 0, sizeof(timers));

	/* Classic scripts are sloppy by default in browsers; strict mode is
	 * opt-in via a "use strict" directive. */
	J = js_newstate(0, 0, 0);
	if (!J)
		return -1;
	js_setreport(J, report_cb);
	js_atpanic(J, panic_cb);
	install_globals(J);
	return 0;
}

void js_browser_stop(void)
{
	if (J)
		js_freestate(J);
	J = 0;
	D = 0;
	current_url = 0;
	panic_armed = 0;
	memset(timers, 0, sizeof(timers));
}

int js_browser_active(void)
{
	return J != 0;
}

static void report_top(js_State *state)
{
	const char *msg;

	js_getproperty(state, -1, "stack");
	if (js_isstring(state, -1)) {
		msg = js_tostring(state, -1);
		if (host.report)
			host.report(msg);
		js_pop(state, 1);
	} else {
		js_pop(state, 1);
		msg = js_trystring(state, -1, "JavaScript error");
		if (host.report)
			host.report(msg);
	}
	js_pop(state, 1);
}

void js_browser_tick(void)
{
	unsigned int now;
	int i, ran = 0;

	if (!J)
		return;
	now = u_ticks_ms();
	for (i = 0; i < JS_TIMERS; i++) {
		if (!timers[i].used ||
		    (int)(now - timers[i].deadline) < 0)
			continue;
		js_setlimit(J, JS_RUN_LIMIT, 0);
		js_getregistry(J, timers[i].ref);
		if (!js_iscallable(J, -1)) {
			js_pop(J, 1);
			js_unref(J, timers[i].ref);
			timers[i].used = 0;
			continue;
		}
		js_pushundefined(J);
		if (js_pcall(J, 0))
			report_top(J);
		else
			js_pop(J, 1);
		if (timers[i].used) {
			if (timers[i].repeat)
				timers[i].deadline = now + timers[i].interval;
			else {
				js_unref(J, timers[i].ref);
				timers[i].used = 0;
			}
		}
		if (++ran >= 4)
			break;
	}
}

int js_browser_run(const char *source, const char *url)
{
	int r;

	if (!J || !D)
		return -1;

	js_setlimit(J, JS_RUN_LIMIT, 0);

	panic_armed = 1;
	if (setjmp(panic_env)) {
		panic_armed = 0;
		if (host.report)
			host.report("uncaught JavaScript exception");
		/* The try/exception stack is unusable after a panic.  Leak the
		 * state; the page heap reset on navigation reclaims it. */
		J = 0;
		D = 0;
		return -1;
	}

	if (js_ploadstring(J, url ? url : "[inline]", source)) {
		report_top(J);
		panic_armed = 0;
		return -1;
	}
	js_pushundefined(J);
	r = js_pcall(J, 0);
	if (r) {
		report_top(J);
		panic_armed = 0;
		return -1;
	}
	js_pop(J, 1);
	panic_armed = 0;
	return 0;
}
