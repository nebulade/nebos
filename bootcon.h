/* Boot console: the earliest on-screen output.
 *
 * Draws 8x8 text straight into the boot framebuffer described at 0x600.  It
 * needs no heap, no paging (the LFB is identity-accessible), no drivers and
 * no shadow buffer, so it can be the first C code in kernel_main.  Purpose:
 * stage tracing on machines without serial, and a last visible screen if the
 * kernel wedges before the WM paints.  The WM's first frame covers it;
 * bootcon_set_enabled(0) stops drawing before that happens.
 */
#ifndef BOOTCON_H
#define BOOTCON_H

#define BOOTCON_LOG_PAGES 8

void bootcon_init(void);
int  bootcon_ok(void);
void bootcon_set_enabled(int on);
void bootcon_puts(const char *s);
void bootcon_dec(unsigned int v);
void bootcon_hex(unsigned int v);
/* One line with the cached mode geometry and LFB address. */
void bootcon_fb_info(void);

/* Allocate the RAM log after the page allocator is up.  The buffer is taken
 * from the frame allocator, not BSS: the low rootfs pack limits _end to
 * 0x60000 and a 16 KB static log would overlap it. */
void bootcon_log_init(void);
/* Everything written through bootcon_puts() after bootcon_log_init(),
 * including text produced after the WM has taken over the framebuffer.  The
 * returned buffer is NUL terminated; *len is the number of valid bytes. */
const char *bootcon_log(unsigned int *len);
int bootcon_log_truncated(void);

#endif
