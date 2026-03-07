/* Example user program: draws "omg!" centered, runs until ESC. Uses kernel API at PROGRAM_API_ADDR. */

#include "prog_api.h"

/* Character cell size used by fbtext (8px wide, 10px row height). */
#define CHAR_WIDTH  8
#define ROW_HEIGHT  10

static const struct prog_api *const api = (const struct prog_api *)PROGRAM_API_ADDR;

/* Pad binary past 1KB so it uses cluster 8+ (reproduces LBA 0x27 read failure on real HW). */
static const char prog_padding[3072] __attribute__((used)) = { 0 };

void _start(void)
{
	(void)prog_padding;  /* keep in binary */
	unsigned int cols, rows, cx, cy;
	const char *msg = "omg!";

	cols = api->get_cols();
	rows = api->get_rows();
	/* Center "omg!" (4 chars): x = (cols*8)/2 - (4*8)/2, y = (rows*10)/2 - 5 */
	cx = (cols * CHAR_WIDTH) / 2 - (4 * CHAR_WIDTH) / 2;
	cy = (rows * ROW_HEIGHT) / 2 - ROW_HEIGHT / 2;

	api->draw_string_xy(cx, cy, msg, 255, 255, 255, 0);

	while (api->poll_key() != IO_KEY_ESC)
		;

	return;
}
