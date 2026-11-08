/* Host smoke test for the vendored MuJS engine. */
#include <stdio.h>
#include <string.h>

#include "mujs.h"

static int reports;

static void report(js_State *J, const char *message)
{
	(void)J;
	printf("mujs report: %s\n", message);
	reports++;
}

int main(void)
{
	js_State *J = js_newstate(0, 0, JS_STRICT);
	int failures = 0;

	if (!J) {
		printf("mujs test: cannot create a state\n");
		return 1;
	}
	js_setreport(J, report);

	if (js_dostring(J, "if (1 + 2 !== 3) throw 'arithmetic';")) {
		printf("mujs test: arithmetic failed\n");
		failures++;
	}
	if (js_dostring(J,
		"var s = 'x'; s += 42;"
		"if (s !== 'x42') throw 'string ' + s;")) {
		printf("mujs test: string failed\n");
		failures++;
	}
	if (js_dostring(J,
		"if (Math.sqrt(9) !== 3) throw 'sqrt';"
		"if (Math.pow(2, 10) !== 1024) throw 'pow';"
		"if (Math.abs(-4) !== 4) throw 'abs';")) {
		printf("mujs test: math failed\n");
		failures++;
	}
	if (js_dostring(J,
		"var d = new Date(0);"
		"if (d.getUTCFullYear() !== 1970 || d.getUTCMonth() !== 0 ||"
		"    d.getUTCDate() !== 1) throw 'date';"
		"if (Date.UTC(2020, 0, 1) !== 1577836800000) throw 'utc';")) {
		printf("mujs test: date failed\n");
		failures++;
	}
	if (js_dostring(J,
		"var o = JSON.parse('{\"a\":[1,2,3]}');"
		"if (o.a.length !== 3 || o.a[2] !== 3) throw 'json parse';"
		"if (JSON.stringify({x: 1}) !== '{\"x\":1}') throw 'json print';")) {
		printf("mujs test: json failed\n");
		failures++;
	}

	/* A thrown error must be reported and must not take the process down. */
	reports = 0;
	if (!js_dostring(J, "throw new Error('expected');")) {
		printf("mujs test: exception not caught\n");
		failures++;
	}
	if (reports != 1) {
		printf("mujs test: expected one report, got %d\n", reports);
		failures++;
	}

	/* The run limit turns an endless loop into a catchable exception. */
	reports = 0;
	js_setlimit(J, 2000, 0);
	if (!js_dostring(J, "while (1) {}")) {
		printf("mujs test: run limit did not fire\n");
		failures++;
	}
	if (reports != 1) {
		printf("mujs test: run limit report count %d\n", reports);
		failures++;
	}

	js_freestate(J);
	if (failures)
		printf("mujs test: %d failure(s)\n", failures);
	else
		printf("mujs test: OK\n");
	return failures != 0;
}
