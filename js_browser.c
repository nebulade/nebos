/* Browser bindings for MuJS: console, alert, document.getElementById,
 * textContent and location.  Kept deliberately small; timers, events and
 * richer DOM mutation belong to the next JavaScript phase. */

#include <setjmp.h>
#include <stdint.h>
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

	if (!D || node >= D->nnode)
		return 0;
	n = &D->nodes[node];
	if (!strcmp(name, "textContent")) {
		node_push_text(state, node);
		return 1;
	}
	if (!strcmp(name, "id")) {
		js_pushstring(state, n->id_off ? D->attrs + n->id_off : "");
		return 1;
	}
	if (!strcmp(name, "tagName")) {
		js_pushstring(state, br_tag_name(n->tag));
		return 1;
	}
	return 0;
}

static int node_put(js_State *state, void *p, const char *name)
{
	unsigned int node = node_of(p);

	if (!D || node >= D->nnode)
		return 0;
	if (!strcmp(name, "textContent")) {
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
	unsigned int handle = br_find_id(D, id);

	if (!handle) {
		js_pushnull(state);
		return;
	}
	/* js_newuserdatax() pops the top value as the userdata prototype; push
	 * a placeholder so it cannot consume the id argument. */
	js_pushnull(state);
	js_newuserdatax(state, "node", (void *)(uintptr_t)handle,
			node_has, node_put, node_delete, 0);
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

static void install_globals(js_State *state)
{
	js_newobject(state);
	js_newcfunction(state, doc_getelementbyid, "getElementById", 1);
	js_setproperty(state, -2, "getElementById");
	js_defglobal(state, "document", JS_DONTENUM);

	js_newobject(state);
	js_newcfunction(state, console_log, "log", 0);
	js_setproperty(state, -2, "log");
	js_defglobal(state, "console", JS_DONTENUM);

	js_newcfunction(state, js_alert, "alert", 1);
	js_defglobal(state, "alert", JS_DONTENUM);

	js_newobject(state);
	js_newcfunction(state, location_get, "get href", 0);
	js_newcfunction(state, location_set, "set href", 1);
	js_defaccessor(state, -3, "href", 0);
	js_defglobal(state, "location", JS_DONTENUM);

	js_pushglobal(state);
	js_defglobal(state, "window", JS_DONTENUM);
	js_pushglobal(state);
	js_defglobal(state, "globalThis", JS_DONTENUM);
}

int js_browser_start(struct br_doc *doc, const struct js_browser_host *h)
{
	if (!doc || !h)
		return -1;
	D = doc;
	host = *h;
	current_url = h->page_url;
	panic_armed = 0;

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
}

int js_browser_active(void)
{
	return J != 0;
}

static void report_top(js_State *state)
{
	const char *msg = js_trystring(state, -1, "JavaScript error");

	if (host.report)
		host.report(msg);
	js_pop(state, 1);
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
