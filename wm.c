/* Window manager: fixed-size draggable terminal windows + a system bar.
 * wm_run() is a normal kernel process; the idle loop keeps input routing,
 * cursor blinking, kexec detection and gfx session cleanup running even when
 * the WM process is not scheduled. */

#include "wm.h"
#include "blit.h"
#include "fbtext.h"
#include "idle.h"
#include "io.h"
#include "kexec.h"
#include "mm.h"
#include "mouse.h"
#include "netkexec.h"
#include "netlog.h"
#include "paging.h"
#include "payload.h"
#include "process.h"
#include "rtc.h"
#include "serial.h"
#include "sys_gfx.h"
#include "text_canvas.h"
#include "thermal.h"
#include "timer.h"
#include "tty.h"
#include "version.h"
#include "vga.h"
#include "vfs.h"

static struct window windows[MAX_WINDOWS];
/* The terminal state is large and only needed for live windows.  Allocate it
 * from the frame allocator (page-aligned) instead of carrying it in BSS: the
 * cell grid is exactly one page, so a ring-3 WM can map just that page. */
#define TERM_STRIDE (2 * PAGE_SIZE)
static struct term *terms[MAX_WINDOWS];
static unsigned int term_counter;
static int wm_full_redraw;

_Static_assert(sizeof(struct term) <= TERM_STRIDE,
	       "struct term no longer fits two pages");
_Static_assert(__builtin_offsetof(struct term, screen) == 0,
	       "term->screen must be the first field");
_Static_assert(__builtin_offsetof(struct text_screen, cells) == 0,
	       "text_screen.cells must be the first field");
_Static_assert(sizeof(((struct text_screen *)0)->cells) == PAGE_SIZE,
	       "the cell grid must be exactly one page");

static int wm_terms_init(void)
{
	unsigned int base = mm_alloc_pages(MAX_WINDOWS * (TERM_STRIDE / PAGE_SIZE));
	unsigned char *p;
	unsigned int i;

	if (!base)
		return -1;
	p = (unsigned char *)base;
	for (i = 0; i < MAX_WINDOWS * TERM_STRIDE; i++)
		p[i] = 0;
	for (i = 0; i < MAX_WINDOWS; i++)
		terms[i] = (struct term *)(base + i * TERM_STRIDE);
	return 0;
}

/* Mouse drag state. */
static int drag_active;
static int drag_window;      /* index into windows[] */
static int drag_dx, drag_dy; /* mouse offset from window origin */
static unsigned int prev_buttons;

/* Content pointer routing for generic canvas windows.  wm_server_poll() is
 * called by the ring-3 WM every loop, so it is the cheapest place to turn the
 * sampled mouse state into press/release edges. */
static unsigned int wm_ptr_buttons;
static int wm_ptr_capture = -1;

/* Ring-3 WM lifecycle.  The kernel only admits the process it spawned; a
 * stale pid cannot keep drawing after a restart. */
static unsigned int wm_expected_pid;
static unsigned int wm_pid;
static unsigned int wm_cr3;
static int wm_attached;
static int wm_frame_active;
static int wm_first_frame;
static int wm_kernel_fallback;
static unsigned int wm_last_start;
static unsigned int wm_last_seen;

static inline unsigned int irq_save_disable(void)
{
	unsigned int flags;
	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	return flags;
}

static inline void irq_restore(unsigned int flags)
{
	__asm__ volatile ("pushl %0; popfl" :: "r"(flags));
}

static inline void invlpg_va(unsigned int va)
{
	__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static void wm_focus_index(unsigned int idx);
static void wm_focus_top(void);
static void wm_close_window(unsigned int idx);
static void wm_clamp_window(struct window *w, int nx, int ny);
static void wm_server_unmap_index(unsigned int idx);
static int wm_window_at(unsigned int px, unsigned int py);

void wm_request_redraw(void)
{
	wm_full_redraw = 1;
}

struct window *wm_focused_window(void)
{
	unsigned int i;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].focused)
			return &windows[i];
	return (void *)0;
}

int wm_is_focused(const struct window *w)
{
	return w->focused;
}

/* The terminal whose screen is `screen`, or NULL: a spawned ring-3 command
 * inherits its terminal's screen pointer, so this maps it back to the window
 * (used by the sys_gfx window-canvas service). */
struct term *wm_term_for_screen(struct text_screen *screen)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term &&
		    &windows[i].term->screen == screen)
			return windows[i].term;
	return 0;
}

/* Pixel size of a window's content area (the size of its gfx canvas). */
int wm_content_size(struct term *t, unsigned int *w, unsigned int *h)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term == t) {
			*w = windows[i].pw - 2;
			*h = windows[i].canvas.px ? windows[i].canvas.h
						  : SCREEN_ROWS * TEXT_CELL_H;
			return 0;
		}
	return -1;
}

/* ---- Ring-3 WM ABI ---------------------------------------------------- */

int wm_server_is_wm(void)
{
	return wm_attached && current_proc()->pid == wm_pid;
}

int wm_server_frame_active(void)
{
	return wm_frame_active;
}

/* Safe point for freeing another window's canvas: the WM is not between
 * SYS_WM_INFO/MAP and PRESENT.  A ring-3 WM sleeps at the end of every loop;
 * the kernel fallback has no cached mappings. */
int wm_server_can_reap(void)
{
	if (wm_frame_active)
		return 0;
	return !wm_attached || process_is_sleeping(wm_pid);
}

static void wm_server_unmap_index(unsigned int idx)
{
	struct window *w = &windows[idx];
	unsigned int va, i;

	if (!wm_cr3 || idx >= MAX_WINDOWS || !w->mapped_pages)
		return;
	va = WM_CONTENT_VA_BASE + idx * WM_CONTENT_SLOT;
	paging_release_user_range(wm_cr3, va, WM_CONTENT_SLOT, 0);
	for (i = 0; i < w->mapped_pages; i++)
		invlpg_va(va + i * PAGE_SIZE);
	w->mapped_kind = WM_KIND_NONE;
	w->mapped_gen = 0;
	w->mapped_pages = 0;
	w->mapped_src = 0;
}

void wm_server_unmap_screen(struct text_screen *screen)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term &&
		    &windows[i].term->screen == screen)
			wm_server_unmap_index(i);
}

/* A dead gfx session can coexist with a newer session on the same screen (the
 * old canvas is freed later).  Only drop the WM mapping when it still points
 * at *this* canvas, so collecting the old session cannot unmap the new one. */
void wm_server_unmap_canvas(struct text_screen *screen, const void *canvas)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term &&
		    &windows[i].term->screen == screen &&
		    windows[i].mapped_kind == WM_KIND_GFX &&
		    windows[i].mapped_src == canvas)
			wm_server_unmap_index(i);
}

static int wm_window_for_pid(unsigned int pid)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].pid == pid)
			return (int)i;
	return -1;
}

static void wm_unmap_back(struct window *w, unsigned int pages)
{
	unsigned int i;

	if (w->owner_cr3 && pages) {
		paging_release_user_range(w->owner_cr3, USER_GFX_BASE,
					  pages * PAGE_SIZE, 0);
		for (i = 0; i < pages; i++)
			invlpg_va(USER_GFX_BASE + i * PAGE_SIZE);
	}
	w->owner_cr3 = 0;
}

static void wm_free_canvas_window(unsigned int idx)
{
	struct window *w = &windows[idx];
	unsigned int i;

	if (!w->canvas_window)
		return;
	wm_unmap_back(w, w->back_pages);
	if (w->back) {
		for (i = 0; i < w->back_pages; i++)
			mm_free_page((unsigned int)w->back + i * PAGE_SIZE);
		w->back = 0;
	}
	w->back_pages = 0;
	if (w->pending_back) {
		for (i = 0; i < w->pending_pages; i++)
			mm_free_page((unsigned int)w->pending_back + i * PAGE_SIZE);
		w->pending_back = 0;
	}
	w->pending_pages = 0;
	w->resize_pending = 0;
	gfx_free(&w->canvas);
	w->canvas_window = 0;
}

int wm_server_win_create(const struct win_create *in, struct win_info *out)
{
	struct window *w;
	struct gfx_canvas *c;
	unsigned short *back;
	unsigned int i, j, pages, cw, ch, base;
	unsigned int flags;

	if (!in || !out)
		return -1;
	if (current_proc()->pid == 0 ||
	    wm_window_for_pid(current_proc()->pid) >= 0)
		return -1;
	cw = in->w;
	ch = in->h;
	if (cw == 0 || ch == 0 || cw > 1024 || ch > 1024)
		return -1;
	if (cw + 2 > vga_fb_width() || ch + WM_TITLE_H + 2 > vga_fb_height())
		return -1;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (!windows[i].used)
			break;
	if (i == MAX_WINDOWS) {
		serial_puts(SERIAL_COM1, "wm: win_create no window slot\n");
		return -1;
	}
	w = &windows[i];
	c = &w->canvas;
	if (gfx_alloc(c, cw, ch) != 0)
		return -1;
	pages = (cw * ch * 2 + PAGE_SIZE - 1) / PAGE_SIZE;
	back = (unsigned short *)mm_alloc_pages(pages);
	if (!back) {
		gfx_free(c);
		return -1;
	}
	for (j = 0; j < cw * ch; j++)
		back[j] = 0;

	flags = irq_save_disable();
	base = current_proc()->cr3;
	for (j = 0; j < pages; j++) {
		if (!paging_map_page(base, USER_GFX_BASE + j * PAGE_SIZE,
				     (unsigned int)back + j * PAGE_SIZE,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			irq_restore(flags);
			paging_release_user_range(base, USER_GFX_BASE,
						  (j ? j : 1) * PAGE_SIZE, 0);
			for (i = 0; i < pages; i++)
				mm_free_page((unsigned int)back + i * PAGE_SIZE);
			gfx_free(c);
			return -1;
		}
		invlpg_va(USER_GFX_BASE + j * PAGE_SIZE);
	}
	irq_restore(flags);

	w->used = 1;
	w->canvas_window = 1;
	w->term = 0;
	w->pid = current_proc()->pid;
	w->owner_cr3 = base;
	w->back = back;
	w->back_pages = pages;
	w->canvas.seq = 1;
	w->canvas.drawn = 0;
	w->pending_back = 0;
	w->pending_pages = 0;
	w->resize_pending = 0;
	w->pw = cw + 2;
	w->ph = ch + WM_TITLE_H + 2;
	w->x = 24 + i * 32;
	w->y = WM_BAR_H + 12 + i * 28;
	if (w->x + w->pw > vga_fb_width())
		w->x = (vga_fb_width() > w->pw) ? vga_fb_width() - w->pw : 0;
	if (w->y + w->ph > vga_fb_height())
		w->y = WM_BAR_H + 4;
	w->focused = 0;
	w->content_gen = 1;
	w->mapped_kind = WM_KIND_NONE;
	w->mapped_gen = 0;
	w->mapped_pages = 0;
	w->mapped_src = 0;
	w->key_head = w->key_tail = 0;
	wait_queue_init(&w->key_wq);
	w->ptr_head = w->ptr_tail = 0;
	for (j = 0; j < sizeof(w->title) - 1 && in->title[j]; j++)
		w->title[j] = in->title[j];
	w->title[j] = '\0';

	wm_focus_index(i);

	out->fb = (void *)USER_GFX_BASE;
	out->width = cw;
	out->height = ch;
	out->id = i;
	return 0;
}

int wm_server_win_present(void)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;
	const unsigned short *src;
	unsigned int n, i;

	if (idx < 0)
		return -1;
	w = &windows[idx];
	if (!w->canvas_window || !w->back || !w->canvas.px ||
	    w->resize_pending)
		return -1;
	src = (const unsigned short *)USER_GFX_BASE;
	n = w->canvas.w * w->canvas.h;
	for (i = 0; i + 1 < n; i += 2)
		((unsigned int *)w->canvas.px)[i / 2] = ((const unsigned int *)src)[i / 2];
	if (n & 1)
		w->canvas.px[n - 1] = src[n - 1];
	w->canvas.seq++;
	return 0;
}

int wm_server_win_close(void)
{
	int idx = wm_window_for_pid(current_proc()->pid);

	if (idx < 0 || !wm_server_can_reap())
		return -1;
	wm_server_unmap_index((unsigned int)idx);
	wm_free_canvas_window((unsigned int)idx);
	windows[idx].used = 0;
	windows[idx].pid = 0;
	windows[idx].focused = 0;
	windows[idx].content_gen++;
	wm_focus_top();
	wm_request_redraw();
	return 0;
}

int wm_server_win_info(struct win_info *out)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;

	if (!out || idx < 0)
		return -1;
	w = &windows[idx];
	if (!w->canvas_window)
		return -1;
	out->fb = (void *)USER_GFX_BASE;
	out->width = w->canvas.w;
	out->height = w->canvas.h;
	out->id = (unsigned int)idx;
	return 0;
}

int wm_server_win_ack_resize(void)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;
	unsigned int i, old_pages;

	if (idx < 0)
		return -1;
	w = &windows[idx];
	if (!w->canvas_window || !w->resize_pending || !w->pending_back)
		return -1;

	old_pages = w->back_pages;
	if (w->owner_cr3)
		paging_release_user_range(w->owner_cr3, USER_GFX_BASE,
					  old_pages * PAGE_SIZE, 0);
	for (i = 0; i < w->pending_pages; i++) {
		if (!paging_map_page(current_proc()->cr3,
				     USER_GFX_BASE + i * PAGE_SIZE,
				     (unsigned int)w->pending_back + i * PAGE_SIZE,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			/* Put the old mapping back; this is a last-resort path. */
			paging_release_user_range(current_proc()->cr3,
						  USER_GFX_BASE,
						  i * PAGE_SIZE, 0);
			for (i = 0; i < old_pages; i++)
				paging_map_page(current_proc()->cr3,
					USER_GFX_BASE + i * PAGE_SIZE,
					(unsigned int)w->back + i * PAGE_SIZE,
					PTE_USER | PTE_WRITABLE | PTE_PRESENT);
			for (i = 0; i < w->pending_pages; i++)
				mm_free_page((unsigned int)w->pending_back +
					     i * PAGE_SIZE);
			w->pending_back = 0;
			w->pending_pages = 0;
			w->resize_pending = 0;
			return -1;
		}
		invlpg_va(USER_GFX_BASE + i * PAGE_SIZE);
	}
	for (i = 0; i < old_pages; i++)
		mm_free_page((unsigned int)w->back + i * PAGE_SIZE);
	w->back = w->pending_back;
	w->back_pages = w->pending_pages;
	w->pending_back = 0;
	w->pending_pages = 0;
	w->resize_pending = 0;
	return 0;
}

int wm_server_win_pointer(struct win_pointer *out)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;
	unsigned int mx, my;
	int cx, cy;

	if (!out || idx < 0 || !windows[idx].canvas_window)
		return -1;
	w = &windows[idx];
	mx = mouse_get_x();
	my = mouse_get_y();
	cx = (int)mx - (int)w->x - 1;
	cy = (int)my - (int)w->y - 1 - WM_TITLE_H;
	out->x = cx;
	out->y = cy;
	out->buttons = mouse_buttons_state();
	out->inside = (cx >= 0 && cy >= 0 &&
		       cx < (int)w->canvas.w && cy < (int)w->canvas.h &&
		       (w->focused || wm_window_at(mx, my) == idx)) ? 1 : 0;
	out->focused = w->focused ? 1 : 0;
	return 0;
}

int wm_server_win_event(struct win_event *out)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;

	if (!out || idx < 0 || !windows[idx].canvas_window)
		return -1;
	w = &windows[idx];
	if (w->ptr_tail == w->ptr_head)
		return 0;
	*out = w->ptr_events[w->ptr_tail];
	w->ptr_tail = (w->ptr_tail + 1) % 8;
	return 1;
}

int wm_server_resize(unsigned int index, unsigned int cw, unsigned int ch)
{
	struct window *w;
	struct gfx_canvas nf;
	unsigned short *nb;
	unsigned int i, pages;

	if (!wm_server_is_wm() || index >= MAX_WINDOWS || !windows[index].used)
		return -1;
	w = &windows[index];
	if (!w->canvas_window || !process_is_alive(w->pid))
		return -1;
	if (cw < 160 || ch < 80 || cw > 1024 || ch > 1024)
		return -1;
	if (cw + 2 > vga_fb_width() || ch + WM_TITLE_H + 2 > vga_fb_height())
		return -1;
	if (gfx_alloc(&nf, cw, ch) != 0)
		return -1;
	pages = (cw * ch * 2 + PAGE_SIZE - 1) / PAGE_SIZE;
	nb = (unsigned short *)mm_alloc_pages(pages);
	if (!nb) {
		gfx_free(&nf);
		return -1;
	}
	for (i = 0; i < cw * ch; i++)
		nb[i] = 0;

	wm_server_unmap_index(index);
	gfx_free(&w->canvas);
	if (w->pending_back) {
		for (i = 0; i < w->pending_pages; i++)
			mm_free_page((unsigned int)w->pending_back + i * PAGE_SIZE);
	}
	w->canvas = nf;
	w->canvas.seq = 1;
	w->canvas.drawn = 0;
	w->pending_back = nb;
	w->pending_pages = pages;
	w->resize_pending = 1;
	w->pw = cw + 2;
	w->ph = ch + WM_TITLE_H + 2;
	w->content_gen++;
	/* The owner is probably parked in SYS_WIN_KEY_WAIT; wake it so it
	 * acknowledges the resize (and reflows its content) immediately. */
	wait_wake_one(&w->key_wq);
	return 0;
}

int wm_server_key_poll(void)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;
	int k;

	if (idx < 0)
		return 0;
	w = &windows[idx];
	if (w->key_tail == w->key_head)
		return 0;
	k = w->keys[w->key_tail];
	w->key_tail = (w->key_tail + 1) % 16;
	return k;
}

/* Block until a key arrives for the caller's window, or `timeout_ms` passes
 * (0 = wait forever).  Returns the key, 0 on timeout/no key, -1 if the caller
 * has no window. */
int wm_server_key_wait(unsigned int timeout_ms)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;
	int k;

	if (idx < 0)
		return -1;
	w = &windows[idx];

	k = wm_server_key_poll();
	if (k)
		return k;
	if (timeout_ms)
		sched_block_timeout(&w->key_wq, timeout_ms);
	else
		sched_block_on(&w->key_wq);
	return wm_server_key_poll();
}

void wm_content_changed(struct text_screen *screen)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term &&
		    &windows[i].term->screen == screen)
			windows[i].content_gen++;
	wm_request_redraw();
}

int wm_server_attach(struct wm_screen_info *out)
{
	struct surface *s;
	unsigned int pages, i, bytes;

	if (!out || !wm_expected_pid ||
	    current_proc()->pid != wm_expected_pid || wm_attached) {
		serial_puts(SERIAL_COM1, "wm: attach rejected (expected ");
		serial_puthex(SERIAL_COM1, wm_expected_pid);
		serial_puts(SERIAL_COM1, " current ");
		serial_puthex(SERIAL_COM1, current_proc()->pid);
		serial_puts(SERIAL_COM1, ")\n");
		return -1;
	}
	if (!vga_shadow_is_active()) {
		serial_puts(SERIAL_COM1, "wm: attach: no shadow buffer\n");
		return -1;
	}
	s = vga_screen_surface();
	if (!s || !s->px)
		return -1;

	bytes = s->pitch * s->h;
	pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	wm_cr3 = current_proc()->cr3;
	for (i = 0; i < pages; i++) {
		unsigned int va = WM_SHADOW_VA + i * PAGE_SIZE;

		if (!paging_map_page(wm_cr3, va,
				     (unsigned int)s->px + i * PAGE_SIZE,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			paging_release_user_range(wm_cr3, WM_SHADOW_VA,
						  i * PAGE_SIZE, 0);
			wm_cr3 = 0;
			return -1;
		}
		invlpg_va(va);
	}

	for (i = 0; i < MAX_WINDOWS; i++) {
		windows[i].mapped_kind = WM_KIND_NONE;
		windows[i].mapped_gen = 0;
		windows[i].mapped_pages = 0;
		windows[i].mapped_src = 0;
	}

	wm_pid = current_proc()->pid;
	wm_attached = 1;
	wm_kernel_fallback = 0;
	wm_frame_active = 0;
	wm_last_seen = get_ticks_ms();
	mouse_set_auto_draw(1);
	wm_full_redraw = 1;

	out->width = s->w;
	out->height = s->h;
	out->pitch = s->pitch;
	out->bpp = s->bpp;
	out->bytes = bytes;
	out->fb = (void *)WM_SHADOW_VA;
	serial_puts(SERIAL_COM1, "wm: ring-3 WM attached\n");
	return 0;
}

/* Render the fallback kernel terminal's cell grid into its RGB565 canvas.
 * The ring-3 terminal owns its own canvas and never comes here.  The canvas
 * stays valid between damage blocks; the WM composites it like any other
 * window content. */
static void wm_text_sync(struct window *w)
{
	struct text_screen *s;
	struct text_canvas tc;
	unsigned int c0, r0, nc, nr;

	if (!w || !w->term || !w->canvas.px)
		return;
	s = &w->term->screen;
	tc.px = w->canvas.px;
	tc.w = w->canvas.w;
	tc.h = w->canvas.h;

	if (!w->text_ready) {
		text_canvas_render(&tc, s->cells, s->cols, s->rows,
				   0, 0, s->cols, s->rows,
				   s->cursor, s->cursor_visible, w->focused);
		w->text_dmg_x = 0;
		w->text_dmg_y = 0;
		w->text_dmg_w = w->canvas.w;
		w->text_dmg_h = w->canvas.h;
		w->text_ready = 1;
	} else if (s->damage_seq != w->canvas.seq) {
		if (!screen_damage_block(s, &c0, &r0, &nc, &nr))
			return;
		text_canvas_render(&tc, s->cells, s->cols, s->rows,
				   c0, r0, nc, nr,
				   s->cursor, s->cursor_visible, w->focused);
		w->text_dmg_x = c0 * TEXT_CELL_W;
		w->text_dmg_y = r0 * TEXT_CELL_H;
		w->text_dmg_w = nc * TEXT_CELL_W;
		w->text_dmg_h = nr * TEXT_CELL_H;
	} else {
		return;
	}
	w->canvas.seq = s->damage_seq;
}

int wm_server_info(unsigned int index, struct wm_window_info *out)
{
	struct window *w;
	struct gfx_canvas *g = 0;
	struct text_screen *s = 0;
	unsigned int i;

	if (!wm_server_is_wm() || !out || index >= MAX_WINDOWS)
		return -1;
	w = &windows[index];
	if (!w->used)
		return 0;

	out->used = 1;
	out->focused = w->focused ? 1 : 0;
	out->pid = w->pid;
	out->x = (int)w->x;
	out->y = (int)w->y;
	out->w = w->pw;
	out->h = w->ph;
	out->dirty = 0;
	out->dirty_seq = 0;
	out->dmg_x = out->dmg_y = out->dmg_w = out->dmg_h = 0;
	out->content_gen = w->content_gen;
	for (i = 0; i < sizeof(out->title) - 1 && w->title[i]; i++)
		out->title[i] = w->title[i];
	out->title[i] = '\0';

	if (w->canvas_window) {
		g = &w->canvas;
	} else if (w->term) {
		s = &w->term->screen;
		g = sys_gfx_canvas_for(s);
		if (!g) {
			wm_text_sync(w);
			if (w->canvas.px)
				g = &w->canvas;
		}
	} else {
		return 1;
	}

	if (g) {
		out->content_w = g->w;
		out->content_h = g->h;
		out->dirty = (g->seq != g->drawn);
		out->dirty_seq = g->seq;
		if (w->term && g == &w->canvas) {
			out->dmg_x = w->text_dmg_x;
			out->dmg_y = w->text_dmg_y;
			out->dmg_w = w->text_dmg_w;
			out->dmg_h = w->text_dmg_h;
		} else {
			out->dmg_x = 0;
			out->dmg_y = 0;
			out->dmg_w = g->w;
			out->dmg_h = g->h;
		}
	}
	return 1;
}

int wm_server_map(unsigned int index, struct wm_map_info *out)
{
	struct window *w;
	struct gfx_canvas *g = 0;
	const void *src;
	unsigned int pages, i, va, stride;

	if (!wm_server_is_wm() || !out || index >= MAX_WINDOWS || !wm_cr3)
		return -1;
	w = &windows[index];
	if (!w->used)
		return -1;
	if (w->canvas_window) {
		g = &w->canvas;
	} else if (w->term) {
		g = sys_gfx_canvas_for(&w->term->screen);
		if (!g) {
			wm_text_sync(w);
			if (w->canvas.px)
				g = &w->canvas;
		}
		if (!g)
			return -1;
	} else {
		return -1;
	}
	src = (const void *)g->px;
	stride = g->w * 2;

	if (w->mapped_kind == WM_KIND_GFX && w->mapped_gen == w->content_gen &&
	    w->mapped_src == src) {
		out->va = (void *)(WM_CONTENT_VA_BASE + index * WM_CONTENT_SLOT);
		out->kind = WM_KIND_GFX;
		out->generation = w->content_gen;
		out->w = g->w;
		out->h = g->h;
		out->stride = stride;
		return 0;
	}

	wm_server_unmap_index(index);
	pages = (g->w * g->h * 2 + PAGE_SIZE - 1) / PAGE_SIZE;
	if (pages * PAGE_SIZE > WM_CONTENT_SLOT)
		return -1;
	va = WM_CONTENT_VA_BASE + index * WM_CONTENT_SLOT;
	for (i = 0; i < pages; i++) {
		if (!paging_map_page(wm_cr3, va + i * PAGE_SIZE,
				     (unsigned int)src + i * PAGE_SIZE,
				     PTE_USER | PTE_PRESENT)) {
			paging_release_user_range(wm_cr3, va,
						  (i ? i : 1) * PAGE_SIZE, 0);
			return -1;
		}
		invlpg_va(va + i * PAGE_SIZE);
	}
	w->mapped_kind = WM_KIND_GFX;
	w->mapped_gen = w->content_gen;
	w->mapped_pages = pages;
	w->mapped_src = src;

	out->va = (void *)va;
	out->kind = WM_KIND_GFX;
	out->w = g->w;
	out->h = g->h;
	out->stride = stride;
	out->generation = w->content_gen;
	return 0;
}

int wm_server_ack(unsigned int index, unsigned int seq)
{
	struct window *w;
	struct gfx_canvas *g;
	struct text_screen *s;

	if (!wm_server_is_wm() || index >= MAX_WINDOWS)
		return -1;
	w = &windows[index];
	if (!w->used)
		return -1;
	if (w->canvas_window) {
		if (seq == w->canvas.seq)
			w->canvas.drawn = seq;
		return 0;
	}
	if (!w->term)
		return -1;
	s = &w->term->screen;
	g = sys_gfx_canvas_for(s);
	if (g) {
		if (seq == g->seq)
			g->drawn = seq;
		return 0;
	}
	if (w->canvas.px && seq == w->canvas.seq)
		w->canvas.drawn = seq;
	if (seq == s->damage_seq)
		screen_clear_damage(s);
	return 0;
}

int wm_server_focus(unsigned int index)
{
	if (!wm_server_is_wm() || index >= MAX_WINDOWS ||
	    !windows[index].used)
		return -1;
	wm_focus_index(index);
	return 0;
}

int wm_server_set_geom(unsigned int index, int x, int y)
{
	if (!wm_server_is_wm() || index >= MAX_WINDOWS ||
	    !windows[index].used)
		return -1;
	wm_clamp_window(&windows[index], x, y);
	return 0;
}

int wm_server_create_term(void)
{
	int idx;

	if (!wm_server_is_wm())
		return -1;
	idx = wm_spawn_terminal();
	return idx;
}

int wm_server_close(unsigned int index)
{
	if (!wm_server_is_wm() || wm_frame_active ||
	    index >= MAX_WINDOWS || !windows[index].used)
		return -1;
	wm_close_window(index);
	return 0;
}

static void wm_queue_ptr_event(struct window *w, unsigned int type,
			       int x, int y, unsigned int buttons)
{
	unsigned int next = (w->ptr_head + 1) % 8;

	if (next == w->ptr_tail)
		return;                 /* full: drop the event, not the app */
	w->ptr_events[w->ptr_head].type = type;
	w->ptr_events[w->ptr_head].x = x;
	w->ptr_events[w->ptr_head].y = y;
	w->ptr_events[w->ptr_head].buttons = buttons;
	w->ptr_head = next;
	/* Wake a window blocked in SYS_WIN_KEY_WAIT so a click (e.g. on the
	 * terminal's scrollbar) is handled now, not on the next poll timeout. */
	wait_wake_one(&w->key_wq);
}

/* Turn the sampled button state into content-relative press/release events for
 * the topmost canvas window.  The kernel WM still owns focus, drag and resize;
 * only clicks that land in the content area are forwarded. */
static void wm_route_pointer(unsigned int mx, unsigned int my,
			     unsigned int buttons)
{
	unsigned int press = buttons & ~wm_ptr_buttons;
	unsigned int release = wm_ptr_buttons & ~buttons;

	if (press & 1) {
		int idx = wm_window_at(mx, my);

		if (idx >= 0) {
			struct window *w = &windows[idx];
			int cx = (int)mx - (int)w->x - 1;
			int cy = (int)my - (int)w->y - 1 - WM_TITLE_H;

			if (w->canvas_window && cx >= 0 && cy >= 0 &&
			    cx < (int)w->canvas.w && cy < (int)w->canvas.h &&
			    !(mx >= w->x + w->pw - 16 &&
			      my >= w->y + w->ph - 16)) {
				wm_queue_ptr_event(w, WIN_EV_POINTER_DOWN,
						   cx, cy, buttons);
				wm_ptr_capture = idx;
			}
		}
	}
	if ((release & 1) && wm_ptr_capture >= 0) {
		struct window *w = &windows[wm_ptr_capture];

		if (w->used && w->canvas_window) {
			int cx = (int)mx - (int)w->x - 1;
			int cy = (int)my - (int)w->y - 1 - WM_TITLE_H;

			wm_queue_ptr_event(w, WIN_EV_POINTER_UP,
					   cx, cy, buttons);
		}
		wm_ptr_capture = -1;
	}
	wm_ptr_buttons = buttons;
}

int wm_server_poll(struct wm_status *out)
{
	if (!wm_server_is_wm() || !out)
		return -1;
	wm_last_seen = get_ticks_ms();
	out->mouse.x = mouse_get_x();
	out->mouse.y = mouse_get_y();
	out->mouse.buttons = mouse_buttons_state();
	wm_route_pointer(out->mouse.x, out->mouse.y, out->mouse.buttons);
	out->full_redraw = wm_full_redraw ? 1 : 0;
	out->kexec_busy = kexec_busy ? 1 : 0;
	out->kexec_progress = kexec_progress;
	out->kexec_total = kexec_total;
	wm_full_redraw = 0;
	return 0;
}

int wm_server_begin(void)
{
	if (!wm_server_is_wm() || wm_frame_active)
		return -1;
	mouse_cursor_erase();
	mouse_set_auto_draw(0);
	wm_frame_active = 1;
	return 0;
}

int wm_server_present(const struct wm_present_info *in)
{
	unsigned int i;

	if (!wm_server_is_wm() || !wm_frame_active || !in)
		return -1;
	for (i = 0; i < in->count && i < 4; i++) {
		int x0 = in->rect[i].x;
		int y0 = in->rect[i].y;
		int x1 = x0 + (int)in->rect[i].w;
		int y1 = y0 + (int)in->rect[i].h;

		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 < 0) x1 = 0;
		if (y1 < 0) y1 = 0;
		if (x1 > (int)vga_fb_width()) x1 = (int)vga_fb_width();
		if (y1 > (int)vga_fb_height()) y1 = (int)vga_fb_height();
		if (x1 > x0 && y1 > y0)
			vga_fb_mark_dirty_rect_xy((unsigned int)y0,
				(unsigned int)(y1 - y0),
				(unsigned int)x0, (unsigned int)x1);
	}
	mouse_redraw_cursor();
	mouse_set_auto_draw(1);
	wm_frame_active = 0;
	/* The copy can be long; let IRQs run.  vga_present_shadow() keeps the
	 * scheduler from switching away while it owns the damage list. */
	__asm__ volatile ("sti" ::: "memory");
	if (in->immediate)
		vga_fb_flush();
	else
		vga_fb_flush_budget(VGA_FLUSH_BUDGET_BYTES);
	sys_gfx_collect();
	if (!wm_first_frame) {
		wm_first_frame = 1;
		serial_puts(SERIAL_COM1, "wm: first frame\n");
	}
	return 0;
}

int wm_server_sync(void)
{
	if (wm_frame_active)
		return -1;
	sys_gfx_collect();
	return 0;
}

static void wm_server_wm_died(void)
{
	unsigned int i;

	if (wm_frame_active) {
		mouse_set_auto_draw(1);
		mouse_redraw_cursor();
	}
	wm_frame_active = 0;
	wm_attached = 0;
	wm_pid = 0;
	wm_cr3 = 0;
	wm_kernel_fallback = 0;
	wm_ptr_buttons = 0;
	wm_ptr_capture = -1;
	for (i = 0; i < MAX_WINDOWS; i++) {
		windows[i].mapped_kind = WM_KIND_NONE;
		windows[i].mapped_gen = 0;
		windows[i].mapped_pages = 0;
		windows[i].mapped_src = 0;
	}
	wm_full_redraw = 1;
	serial_puts(SERIAL_COM1, "wm: ring-3 WM stopped; restarting\n");
}

void wm_server_start(void)
{
	struct vfs_file f;
	struct vfs_mount *m = 0;
	unsigned int flags;
	int pid;

	if (wm_expected_pid && process_is_alive(wm_expected_pid))
		return;
	if (wm_last_start && get_ticks_ms() - wm_last_start < 1000)
		return;
	wm_last_start = get_ticks_ms();

	if (vfs_load_file("/ramfs/bin/wm", &f, &m) == 0) {
		flags = irq_save_disable();
		pid = process_create_user("wm", f.data, f.size, "", "/");
		if (pid > 0)
			wm_expected_pid = (unsigned int)pid;
		irq_restore(flags);
		vfs_release_file(m, &f);
		if (pid > 0) {
			process_set_weight((unsigned int)pid, 16);
			wm_kernel_fallback = 0;
			serial_puts(SERIAL_COM1, "wm: ring-3 WM started\n");
			return;
		}
		serial_puts(SERIAL_COM1, "wm: cannot load /ramfs/bin/wm\n");
	} else {
		serial_puts(SERIAL_COM1, "wm: /ramfs/bin/wm not found\n");
	}

	/* Temporary fallback during the transition: the old kernel WM. */
	flags = irq_save_disable();
	pid = process_create("wm", wm_run);
	if (pid > 0)
		wm_expected_pid = (unsigned int)pid;
	irq_restore(flags);
	if (pid > 0) {
		process_set_weight((unsigned int)pid, 16);
		wm_kernel_fallback = 1;
		serial_puts(SERIAL_COM1, "wm: kernel WM fallback\n");
	}
}

void wm_server_supervise(void)
{
	unsigned int i, now = get_ticks_ms();

	/* Watchdog: a WM that never attaches, or stops polling for several
	 * seconds, is treated like a crash so a wedged UI can recover. */
	if (wm_expected_pid && process_is_alive(wm_expected_pid) &&
	    !wm_kernel_fallback &&
	    !process_is_sleeping(wm_expected_pid)) {
		/* A sleeping WM is usually blocked on the BKL or its normal
		 * 10 ms poll sleep; do not treat a long disk scan as a hang. */
		if ((wm_attached && now - wm_last_seen > 15000) ||
		    (!wm_attached && now - wm_last_start > 10000)) {
			serial_puts(SERIAL_COM1, "wm: watchdog, killing WM\n");
			process_kill(wm_expected_pid);
		}
	}

	if (wm_expected_pid && !process_is_alive(wm_expected_pid)) {
		wm_server_wm_died();
		wm_expected_pid = 0;
	}

	/* Owner processes that exited close their windows.  Only at a safe
	 * point: the WM may still be between INFO/MAP and PRESENT. */
	if (wm_server_can_reap()) {
		for (i = 0; i < MAX_WINDOWS; i++) {
			struct window *w = &windows[i];

			if (!w->used || !w->pid || process_is_alive(w->pid))
				continue;
			if (w->canvas_window)
				w->owner_cr3 = 0;  /* cr3 is gone/reaped */
			wm_close_window(i);
		}
	}

	if (wm_expected_pid && process_is_alive(wm_expected_pid))
		return;
	wm_server_start();
}

void wm_server_init(void)
{
	unsigned int i;
	int have_terms = (wm_terms_init() == 0);

	if (!have_terms)
		serial_puts(SERIAL_COM1, "wm: no memory for terminal state\n");
	for (i = 0; i < MAX_WINDOWS; i++) {
		windows[i].used = 0;
		windows[i].term = terms[i];
		windows[i].mapped_kind = WM_KIND_NONE;
		windows[i].mapped_gen = 0;
		windows[i].mapped_pages = 0;
		windows[i].mapped_src = 0;
	}
	term_counter = 0;
	wm_full_redraw = 1;
	drag_active = 0;
	prev_buttons = 0;
	wm_ptr_buttons = 0;
	wm_ptr_capture = -1;
	wm_expected_pid = 0;
	wm_pid = 0;
	wm_cr3 = 0;
	wm_attached = 0;
	wm_frame_active = 0;
	wm_first_frame = 0;
	wm_kernel_fallback = 0;
	/* The first terminal is spawned by kernel_main after the boot storage
	 * scan: no user command can touch the disks while the mount table is
	 * being rebuilt.  The desktop itself does not need a window. */
}

static int wm_point_in_rect(unsigned int px, unsigned int py,
	unsigned int rx, unsigned int ry, unsigned int rw, unsigned int rh)
{
	return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void wm_make_title(struct window *w, unsigned int n)
{
	char num[4];
	unsigned int i = 0, k = 0;
	w->title[k++] = 't'; w->title[k++] = 'e'; w->title[k++] = 'r';
	w->title[k++] = 'm'; w->title[k++] = ' ';
	do {
		num[i++] = (char)('0' + (n % 10));
		n /= 10;
	} while (n && i < 3);
	while (i)
		w->title[k++] = num[--i];
	w->title[k] = '\0';
}

int wm_spawn_terminal(void)
{
	unsigned int i, fbw = vga_fb_width();
	struct window *w;
	struct term *t;
	int pid;
	unsigned int n;

	/* Do not start a term that has nowhere to go: it would create a tty
	 * and then exit at SYS_WIN_CREATE, which looks like a dead button. */
	for (i = 0; i < MAX_WINDOWS; i++)
		if (!windows[i].used)
			break;
	if (i == MAX_WINDOWS || tty_free_count() <= 0) {
		serial_puts(SERIAL_COM1, "wm: no term slot (windows/free tty)\n");
		return -1;
	}

	/* Preferred path: the ring-3 terminal owns a canvas window and creates
	 * its own tty.  The in-kernel cell terminal below is only a fallback for
	 * a pack without /ramfs/bin/term. */
	{
		struct vfs_file f;
		struct vfs_mount *m = 0;

		if (vfs_load_file("/ramfs/bin/term", &f, &m) == 0) {
			unsigned int flags = irq_save_disable();

			pid = process_create_user_tty("term", f.data, f.size,
						      "", "/", 0);
			irq_restore(flags);
			vfs_release_file(m, &f);
			if (pid > 0) {
				process_set_weight((unsigned int)pid, 12);
				wm_request_redraw();
				return 0;
			}
			serial_puts(SERIAL_COM1, "wm: cannot start ring-3 term\n");
		}
	}

	for (i = 0; i < MAX_WINDOWS; i++)
		if (!windows[i].used && terms[i])
			break;
	if (i == MAX_WINDOWS)
		return -1;

	w = &windows[i];
	t = terms[i];

	screen_init(&t->screen);
	t->screen.cursor_visible = 1;
	t->key_head = t->key_tail = 0;
	wait_queue_init(&t->key_wq);
	t->history_count = 0;
	t->history_next = 0;
	t->cwd[0] = '/';
	t->cwd[1] = '\0';

	w->used = 1;
	w->term = t;
	w->canvas_window = 0;
	if (gfx_alloc(&w->canvas, SCREEN_COLS * TEXT_CELL_W,
		      SCREEN_ROWS * TEXT_CELL_H) != 0) {
		serial_puts(SERIAL_COM1, "wm: no text canvas\n");
		w->used = 0;
		w->term = 0;
		return -1;
	}
	w->canvas.drawn = 0xFFFFFFFFu;
	w->text_ready = 0;
	w->text_dmg_x = 0;
	w->text_dmg_y = 0;
	w->text_dmg_w = w->canvas.w;
	w->text_dmg_h = w->canvas.h;
	w->back = 0;
	w->back_pages = 0;
	w->owner_cr3 = 0;
	w->key_head = w->key_tail = 0;
	wait_queue_init(&w->key_wq);
	w->pid = 0;
	w->focused = 0;
	w->pw = SCREEN_COLS * TEXT_CELL_W + 2;
	w->ph = WM_TITLE_H + SCREEN_ROWS * TEXT_CELL_H + 2;
	w->content_gen = 1;
	w->mapped_kind = WM_KIND_NONE;
	w->mapped_gen = 0;
	w->mapped_pages = 0;
	w->mapped_src = 0;

	/* Cascade position, clamped to the desktop area. */
	n = term_counter % 5;
	w->x = 24 + n * 32;
	w->y = WM_BAR_H + 12 + n * 28;
	if (w->x + w->pw > fbw)
		w->x = (fbw > w->pw) ? fbw - w->pw : 0;
	if (w->y + w->ph > vga_fb_height())
		w->y = WM_BAR_H + 4;

	term_counter++;
	wm_make_title(w, term_counter);

	pid = process_create_ptr("term", term_entry, t);
	if (pid < 0) {
		gfx_free(&w->canvas);
		w->used = 0;
		w->term = 0;
		return -1;
	}
	process_set_weight((unsigned int)pid, 12);
	w->pid = (unsigned int)pid;

	wm_focus_index(i);
	return (int)i;
}

static void wm_focus_index(unsigned int idx)
{
	unsigned int i;
	if (windows[idx].used && windows[idx].focused)
		return;
	for (i = 0; i < MAX_WINDOWS; i++) {
		int now = windows[i].used && (i == idx);

		if (!windows[i].used || windows[i].focused == now)
			continue;
		windows[i].focused = now;
		/* The terminal cursor lives in the text canvas, so a focus
		 * change must repaint that cell in both windows. */
		if (windows[i].term && windows[i].canvas.px)
			screen_damage_cursor(&windows[i].term->screen);
	}
	wm_request_redraw();
}

static void wm_focus_top(void)
{
	unsigned int i;
	/* With no explicit z-order the highest used slot is painted last, so
	 * it is the topmost window left after a close. */
	for (i = MAX_WINDOWS; i-- > 0; )
		if (windows[i].used) {
			wm_focus_index(i);
			return;
		}
}

static void wm_close_window(unsigned int idx)
{
	struct window *w = &windows[idx];

	if (wm_ptr_capture == (int)idx)
		wm_ptr_capture = -1;
	/* A terminal/key reader blocked on this window must not sleep through
	 * the close; process_kill() also removes it from the queue. */
	wait_wake_all(&w->key_wq);
	if (w->term)
		wait_wake_all(&w->term->key_wq);
	if (w->term) {
		sys_gfx_kill_for_screen(&w->term->screen); /* stop a demo */
		if (w->pid)
			process_kill(w->pid);
		/* Free the dead session before this term/window slot can be
		 * reused, so a late collect cannot unmap a new window that
		 * lands on the same screen. */
		sys_gfx_collect();
	} else if (w->canvas_window) {
		if (w->pid)
			process_kill(w->pid);
		wm_server_unmap_index(idx);
		wm_free_canvas_window(idx);
	} else if (w->pid) {
		process_kill(w->pid);
	}
	wm_server_unmap_index(idx);
	w->used = 0;
	w->pid = 0;
	w->focused = 0;
	w->content_gen++;
	w->mapped_kind = WM_KIND_NONE;
	w->mapped_gen = 0;
	w->mapped_pages = 0;
	w->mapped_src = 0;
	if (w->term && w->canvas.px)
		gfx_free(&w->canvas);
	w->text_ready = 0;
	wm_focus_top();
	wm_request_redraw();
}

/* Topmost window containing (px,py), or -1. */
static int wm_window_at(unsigned int px, unsigned int py)
{
	unsigned int i;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].focused &&
		    wm_point_in_rect(px, py, windows[i].x, windows[i].y,
		    	windows[i].pw, windows[i].ph))
			return (int)i;
	/* Inactive windows are painted in slot order, so the highest slot is
	 * the topmost one under the cursor. */
	for (i = MAX_WINDOWS; i-- > 0; )
		if (windows[i].used && !windows[i].focused &&
		    wm_point_in_rect(px, py, windows[i].x, windows[i].y,
		    	windows[i].pw, windows[i].ph))
			return (int)i;
	return -1;
}

static void wm_clamp_window(struct window *w, int nx, int ny)
{
	unsigned int fbw = vga_fb_width();
	unsigned int fbh = vga_fb_height();
	int maxx = (int)fbw - (int)w->pw;
	int maxy = (int)fbh - (int)w->ph;
	if (maxx < 0)
		maxx = 0;
	if (maxy < (int)WM_BAR_H)
		maxy = (int)WM_BAR_H;
	if (nx < 0)
		nx = 0;
	else if (nx > maxx)
		nx = maxx;
	if (ny < (int)WM_BAR_H)
		ny = (int)WM_BAR_H;
	else if (ny > maxy)
		ny = maxy;
	w->x = (unsigned int)nx;
	w->y = (unsigned int)ny;
}

/* Fill the intersection of rect (rx,ry,rw,rh) with clip rect (cx,cy,cw,ch). */
static void wm_fill_rect_clip(unsigned int rx, unsigned int ry, unsigned int rw, unsigned int rh,
			      unsigned int cx, unsigned int cy, unsigned int cw, unsigned int ch,
			      unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int x0 = (rx > cx) ? rx : cx;
	unsigned int y0 = (ry > cy) ? ry : cy;
	unsigned int x1 = (rx + rw < cx + cw) ? rx + rw : cx + cw;
	unsigned int y1 = (ry + rh < cy + ch) ? ry + rh : cy + ch;
	if (x1 > x0 && y1 > y0)
		vga_fill_rect(x0, y0, x1 - x0, y1 - y0, r, g, b);
}

static void wm_draw_window_clip(struct window *w, unsigned int cx, unsigned int cy,
				unsigned int cw, unsigned int ch);

/* Repaint the desktop rectangle (x, y, w, h): background, then every
   intersecting window back-to-front (focused last), then mark it dirty.  The
   caller handles the mouse cursor.  The rect must lie below the system bar
   (windows are clamped to y >= WM_BAR_H, so the bar is never overlapped and
   needs no clip variant). */
static void wm_redraw_rect(unsigned int x, unsigned int y,
			   unsigned int w, unsigned int h)
{
	unsigned int i;

	if (w == 0 || h == 0)
		return;
	vga_fill_rect(x, y, w, h, 32, 32, 48);
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct window *w2 = &windows[i];

		if (w2->used && !w2->focused &&
		    x < w2->x + w2->pw && x + w > w2->x &&
		    y < w2->y + w2->ph && y + h > w2->y)
			wm_draw_window_clip(w2, x, y, w, h);
	}
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct window *w2 = &windows[i];

		if (w2->used && w2->focused &&
		    x < w2->x + w2->pw && x + w > w2->x &&
		    y < w2->y + w2->ph && y + h > w2->y)
			wm_draw_window_clip(w2, x, y, w, h);
	}
	vga_fb_mark_dirty_rect_xy(y, h, x, x + w);
}

/* Move a window in the shadow buffer: copy its rect, redraw the exposed
   sliver, keep the cursor on top, and flush the swept band immediately so a
   drag step is always one consistent frame (a budgeted flush would copy only
   the top rows of the band per iteration and the window would appear to
   stretch while the mouse keeps moving). */
static void wm_drag_shadow(struct window *w, unsigned int ox, unsigned int oy,
			   unsigned int nx, unsigned int ny)
{
	int dx = (int)nx - (int)ox;
	int dy = (int)ny - (int)oy;

	mouse_cursor_erase();
	blit->copy(nx, ny, ox, oy, w->pw, w->ph);

	if (dy > 0)
		wm_redraw_rect(ox, oy, w->pw, (unsigned int)dy);
	else if (dy < 0)
		wm_redraw_rect(ox, ny + w->ph, w->pw, (unsigned int)(-dy));
	if (dx > 0)
		wm_redraw_rect(ox, oy, (unsigned int)dx, w->ph);
	else if (dx < 0)
		wm_redraw_rect(nx + w->pw, oy, (unsigned int)(-dx), w->ph);

	/* Only the moved window's new rect needs marking: the repaints above
	   cover the exposed slivers exactly, and the cursor marks itself.  The
	   damage list merges these when the step is small (the union is then
	   the swept hull anyway) and keeps them apart after a big jump. */
	vga_fb_mark_dirty_rect_xy(w->y, w->ph, w->x, w->x + w->pw);
	mouse_redraw_cursor();
	vga_fb_flush();
}

static void wm_handle_mouse(void)
{
	unsigned int buttons = mouse_buttons_state();
	unsigned int mx = mouse_get_x();
	unsigned int my = mouse_get_y();
	int left_down = (buttons & 1) != 0;
	int left_was_down = (prev_buttons & 1) != 0;

	if (left_down && !left_was_down) {
		/* Press. */
		if (wm_point_in_rect(mx, my, WM_BTN_X, WM_BTN_Y, WM_BTN_W, WM_BTN_H)) {
			wm_spawn_terminal();
		} else {
			int idx = wm_window_at(mx, my);
			if (idx >= 0) {
				struct window *w = &windows[idx];
				wm_focus_index((unsigned int)idx);
				/* Close button? */
				if (wm_point_in_rect(mx, my,
				    w->x + w->pw - 1 - WM_CLOSE_W, w->y + 1,
				    WM_CLOSE_W, WM_TITLE_H)) {
					wm_close_window((unsigned int)idx);
				} else if (my < w->y + WM_TITLE_H) {
					/* Title bar: start drag. */
					drag_active = 1;
					drag_window = idx;
					drag_dx = (int)mx - (int)w->x;
					drag_dy = (int)my - (int)w->y;
				}
			}
		}
	} else if (left_down && left_was_down && drag_active) {
		struct window *w = &windows[drag_window];
		if (w->used) {
			unsigned int ox = w->x, oy = w->y;
			wm_clamp_window(w, (int)mx - drag_dx, (int)my - drag_dy);
			if (ox != w->x || oy != w->y)
				wm_drag_shadow(w, ox, oy, w->x, w->y);
		} else {
			drag_active = 0;
		}
	} else if (!left_down && left_was_down) {
		drag_active = 0;
	}

	prev_buttons = buttons;
}

void wm_pump_keyboard(void)
{
	struct window *fw = wm_focused_window();
	static unsigned int last_hw_poll;
	unsigned int now = get_ticks_ms();
	int n;

	if (io_keyboard_raw())
		return;   /* an inline command is polling for keys itself */

	if (!fw)
		return;

	for (n = 0; n < 8; n++) {
		int k = io_poll_key();
		if (!k && (now - last_hw_poll) >= (io_keyboard_irq_masked() ? 5 : 50)) {
			/* Fallback paths: IRQ1 was storm-masked, or no IRQ is
			 * delivering; sample the controller directly (faster
			 * when masked, so fast typing isn't dropped). */
			k = io_poll_key_with_hw();
			last_hw_poll = now;
		}
		if (!k)
			break;
		if (fw->term) {
			/* A graphics command owns its window's keys (ESC etc.);
			 * Ctrl+C always reaches the terminal so its foreground
			 * kill keeps working. */
			if (k == IO_KEY_CTRL_C ||
			    !sys_gfx_push_key(&fw->term->screen, k)) {
				term_key_push(fw->term, k);
				term_key_wake(fw->term);
			}
		} else if (fw->canvas_window) {
			unsigned int next = (fw->key_head + 1) % 16;

			if (next != fw->key_tail) {
				fw->keys[fw->key_head] = k;
				fw->key_head = next;
			}
			wait_wake_one(&fw->key_wq);
		}
	}
}

static void wm_draw_bar(void)
{
	unsigned int fbw = vga_fb_width();
	char dt[20];

	/* Banner (light green). */
	fbtext_draw_string_xy(4, 5, OS_NAME " v" OS_VERSION, 85, 255, 85, 0);

	/* "+ New" button: purple box, white text. */
	vga_fill_rect(WM_BTN_X, WM_BTN_Y, WM_BTN_W, WM_BTN_H, 150, 60, 210);
	fbtext_draw_string_xy(WM_BTN_X + (WM_BTN_W - 3 * 8) / 2, WM_BTN_Y + 3,
			      "New", 255, 255, 255, 1);

	if (kexec_busy) {
		/* Kexec progress, in place of the clock. */
		static const char msg[] = "loading new kernel over serial";
		char buf[48];
		unsigned int n = 0, dots, i;
		while (msg[n])
			buf[n] = msg[n], n++;
		dots = (kexec_total > 0) ? (kexec_progress * 12 / kexec_total) : 0;
		for (i = 0; i < 12; i++)
			buf[n + i] = (i < dots) ? '.' : ' ';
		n += 12;
		buf[n] = '\0';
		fbtext_draw_string_xy(fbw - n * 8 - 4, 5, buf, 255, 200, 100, 0);
	} else {
		int temp = thermal_max_c();

		/* Live clock (light green, right-aligned). */
		rtc_read_datetime(dt);
		fbtext_draw_string_xy(fbw - 19 * 8 - 4, 5, dt, 85, 255, 85, 0);
		if (temp >= 0) {
			char tb[12];
			unsigned int tn = 0;
			unsigned int v = (unsigned int)temp;
			int x;

			tb[tn++] = 'C';
			tb[tn++] = 'P';
			tb[tn++] = 'U';
			tb[tn++] = ' ';
			if (v >= 100)
				tb[tn++] = (char)('0' + (v / 100) % 10);
			if (v >= 10)
				tb[tn++] = (char)('0' + (v / 10) % 10);
			tb[tn++] = (char)('0' + v % 10);
			tb[tn++] = 'C';
			tb[tn] = '\0';
			x = (int)fbw - 19 * 8 - 4 - 8 - (int)tn * 8;
			if (x >= WM_BTN_X + (int)WM_BTN_W + 4) {
				if (temp >= 80)
					fbtext_draw_string_xy((unsigned int)x, 5, tb,
							      255, 80, 80, 0);
				else if (temp >= 60)
					fbtext_draw_string_xy((unsigned int)x, 5, tb,
							      255, 210, 80, 0);
				else
					fbtext_draw_string_xy((unsigned int)x, 5, tb,
							      85, 255, 85, 0);
			}
		}
	}

	/* Rainbow separator. */
	fbtext_draw_rainbow_separator();
}

/* Draw window w clipped to (cx, cy, cw, ch).  Chrome uses wm_fill_rect_clip,
 * content the cells/canvas clip variants, so pixels outside the clip are
 * untouched. */
static void wm_draw_window_clip(struct window *w, unsigned int cx, unsigned int cy,
				unsigned int cw, unsigned int ch)
{
	struct text_screen *s;
	struct gfx_canvas *g;

	if (!w->term)
		return;   /* generic canvas windows are drawn by the ring-3 WM */
	s = &w->term->screen;
	g = sys_gfx_canvas_for(s);
	if (!g) {
		wm_text_sync(w);
		if (w->canvas.px)
			g = &w->canvas;
	}
	unsigned int bx = w->x + w->pw - 1 - WM_CLOSE_W;
	unsigned int cony = w->y + 1 + WM_TITLE_H;

	/* Border. */
	wm_fill_rect_clip(w->x, w->y, w->pw, w->ph, cx, cy, cw, ch, 200, 200, 200);

	/* Title bar. */
	if (w->focused)
		wm_fill_rect_clip(w->x + 1, w->y + 1, w->pw - 2, WM_TITLE_H,
			cx, cy, cw, ch, 70, 90, 150);
	else
		wm_fill_rect_clip(w->x + 1, w->y + 1, w->pw - 2, WM_TITLE_H,
			cx, cy, cw, ch, 100, 100, 100);

	/* Title text. */
	fbtext_draw_string_xy_clip(w->x + 4, w->y + 3, w->title, 255, 255, 255,
		cx, cy, cw, ch);

	/* Close button. */
	wm_fill_rect_clip(bx, w->y + 1, WM_CLOSE_W, WM_TITLE_H,
		cx, cy, cw, ch, 198, 64, 64);
	fbtext_draw_string_xy_clip(bx + 4, w->y + 3, "x", 255, 255, 255,
		cx, cy, cw, ch);

	/* Content: the window's canvas (text canvas or a gfx command). */
	wm_fill_rect_clip(w->x + 1, cony, w->pw - 2,
		g ? g->h : SCREEN_ROWS * TEXT_CELL_H,
		cx, cy, cw, ch, 0, 0, 0);
	if (g)
		blit->canvas(g, w->x + 1, cony, cx, cy, cw, ch);
}

static void wm_draw_window(struct window *w)
{
	wm_draw_window_clip(w, w->x, w->y, w->pw, w->ph);
}

static void wm_render_full(void)
{
	unsigned int fbw = vga_fb_width();
	unsigned int fbh = vga_fb_height();
	unsigned int i;

	/* Erase the mouse cursor so the scene is drawn onto a clean background. */
	mouse_cursor_erase();

	/* Desktop background. */
	vga_fill_rect(0, 0, fbw, fbh, 32, 32, 48);

	/* Windows back-to-front (focused drawn last = on top). */
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && !windows[i].focused)
			wm_draw_window(&windows[i]);
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].focused)
			wm_draw_window(&windows[i]);

	/* System bar always on top. */
	wm_draw_bar();

	/* Mouse cursor on top of everything. */
	mouse_redraw_cursor();
}

static void wm_render_bar(void)
{
	unsigned int fbw = vga_fb_width();
	mouse_cursor_erase();
	vga_fill_rect(0, 0, fbw, WM_BAR_H, 32, 32, 48);
	wm_draw_bar();
	mouse_redraw_cursor();
}

static void wm_redraw_if_needed(void)
{
	unsigned int i;
	static unsigned int last_sec = 0xFFFFFFFF;
	static unsigned int last_kexec_tick;
	unsigned int now_ms = get_ticks_ms();
	unsigned int now_sec = now_ms / 1000;
	int kexec_changed = kexec_busy && (now_ms - last_kexec_tick >= 100);
	int bar_changed;

	if (kexec_changed)
		last_kexec_tick = now_ms;
	bar_changed = (now_sec != last_sec) || kexec_changed;

	if (wm_full_redraw) {
		unsigned int seq_before[MAX_WINDOWS];

		for (i = 0; i < MAX_WINDOWS; i++) {
			struct gfx_canvas *g = (windows[i].used && windows[i].term) ?
				sys_gfx_canvas_for(&windows[i].term->screen) : 0;

			seq_before[i] = g ? g->seq : 0;
		}
		wm_render_full();
		for (i = 0; i < MAX_WINDOWS; i++)
			if (windows[i].used && windows[i].term) {
				struct gfx_canvas *g = sys_gfx_canvas_for(&windows[i].term->screen);

				screen_clear_damage(&windows[i].term->screen);
				if (g)
					g->drawn = seq_before[i];   /* keep a frame
						presented during the render */
			}
		wm_full_redraw = 0;
		last_sec = now_sec;
		return;
	}

	if (bar_changed) {
		wm_render_bar();
		last_sec = now_sec;
	}

	/* Any window whose content changed (focused or not): repaint just its
	   damaged cell block (a canvas window repaints its whole content)
	   through the damage-rect compositor. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct text_screen *s;
		struct gfx_canvas *g;
		unsigned int seq;

		if (!windows[i].used || !windows[i].term)
			continue;
		s = &windows[i].term->screen;
		g = sys_gfx_canvas_for(s);
		if (!g) {
			wm_text_sync(&windows[i]);
			if (windows[i].canvas.px)
				g = &windows[i].canvas;
		}
		if (!g || g->seq == g->drawn)
			continue;
		seq = g->seq;   /* capture before compositing */
		mouse_cursor_erase();
		if (g == &windows[i].canvas)
			wm_redraw_rect(windows[i].x + 1 +
				       (int)windows[i].text_dmg_x,
				windows[i].y + 1 + WM_TITLE_H +
				       (int)windows[i].text_dmg_y,
				windows[i].text_dmg_w, windows[i].text_dmg_h);
		else
			wm_redraw_rect(windows[i].x + 1,
				windows[i].y + 1 + WM_TITLE_H,
				g->w, g->h);
		mouse_redraw_cursor();
		screen_clear_damage(s);
		g->drawn = seq;
	}
}

void wm_blink_cursor(void)
{
	static unsigned int last_cursor_tick;
	struct window *fw = wm_focused_window();
	unsigned int now = get_ticks_ms();

	if (!fw || !fw->term)
		return;
	if (sys_gfx_canvas_for(&fw->term->screen))
		return;   /* no text cursor over a graphics command */
	if (now - last_cursor_tick >= CURSOR_MS) {
		last_cursor_tick = now;
		fw->term->screen.cursor_visible = !fw->term->screen.cursor_visible;
		screen_damage_cursor(&fw->term->screen);
	}
}

void wm_handle_kexec(void)
{
	if (kexec_busy)
		return;
	/* A command pack for /ramfs (checked before kexec: both start with
	 * 'K', and the kexec detector drops anything that is not KXE2). */
	if (payload_pending()) {
		kexec_busy = 1;
		if (process_create("payload", program_payload_main) < 0)
			kexec_busy = 0;
		return;
	}
	if (kexec_pending()) {
		kexec_busy = 1;
		if (process_create("kexec", program_kexec_main) < 0)
			kexec_busy = 0;
		return;
	}
	/* Same over UDP: the host's `make net-dev` is picked up from here. */
	if (netkexec_pending()) {
		kexec_busy = 1;
		if (process_create("netkexec", program_netkexec_main) < 0)
			kexec_busy = 0;
	}
}

void wm_run(void)
{
	for (;;) {
		wm_handle_mouse();
		wm_redraw_if_needed();
		vga_fb_flush_budget(VGA_FLUSH_BUDGET_BYTES);
		/* This is the kernel-WM fallback, i.e. a kernel thread holding
		 * the BKL.  Do not sleep on it: release it around the idle wait
		 * so ring-3 syscalls on the APs can still run. */
		bkl_pause();
		cpu_idle();
		bkl_resume();
	}
}
