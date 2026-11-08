/* Browser bindings for the embedded MuJS interpreter.
 *
 * The host owns the document and the UI; this layer exposes the DOM surface
 * (elements, attributes, selectors, classList/dataset/style, innerHTML,
 * timers) plus document/window/location/navigator globals, and leaves the
 * node tree operations to browser_html.c. */
#ifndef JS_BROWSER_H
#define JS_BROWSER_H

struct br_doc;

struct js_browser_host {
	void (*status)(const char *msg, int error);
	void (*report)(const char *msg);
	void (*navigate)(const char *url);
	void (*changed)(void);
	const char *page_url;   /* location.href for every script on the page */
	unsigned int width;     /* viewport size for window.inner* */
	unsigned int height;
};

/* Create a state for this document.  Returns 0 on success. */
int js_browser_start(struct br_doc *doc, const struct js_browser_host *host);
void js_browser_stop(void);

/* Run one script.  `url` is used in error messages and by location.href. */
int js_browser_run(const char *source, const char *url);

/* Run due setTimeout/setInterval callbacks; call once per UI loop. */
void js_browser_tick(void);

int js_browser_active(void);

#endif
