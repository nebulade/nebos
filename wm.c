/* Kernel window server for the ring-3 WM: window bookkeeping, canvas
 * mapping/presentation and input routing.  There is no in-kernel WM. */

#include "wm.h"
#include "bootcon.h"
#include "io.h"
#include "kexec.h"
#include "mm.h"
#include "mouse.h"
#include "netkexec.h"
#include "netlog.h"
#include "paging.h"
#include "payload.h"
#include "process.h"
#include "serial.h"
#include "timer.h"
#include "vga.h"
#include "vfs.h"

static struct window windows[MAX_WINDOWS];
static int wm_full_redraw;

/* Content pointer routing for canvas windows.  wm_server_poll() is
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
static int wm_boot_ready;
static int wm_boot_logged;
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


/* A dead gfx session can coexist with a newer session on the same screen (the
 * old canvas is freed later).  Only drop the WM mapping when it still points
 * at *this* canvas, so collecting the old session cannot unmap the new one. */

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
		paging_release_user_range(w->owner_cr3, USER_WIN_BASE,
					  pages * PAGE_SIZE, 0);
		for (i = 0; i < pages; i++)
			invlpg_va(USER_WIN_BASE + i * PAGE_SIZE);
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
	w->canvas_dmg_w = w->canvas_dmg_h = 0;
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
		if (!paging_map_page(base, USER_WIN_BASE + j * PAGE_SIZE,
				     (unsigned int)back + j * PAGE_SIZE,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			irq_restore(flags);
			paging_release_user_range(base, USER_WIN_BASE,
						  (j ? j : 1) * PAGE_SIZE, 0);
			for (i = 0; i < pages; i++)
				mm_free_page((unsigned int)back + i * PAGE_SIZE);
			gfx_free(c);
			return -1;
		}
		invlpg_va(USER_WIN_BASE + j * PAGE_SIZE);
	}
	irq_restore(flags);

	w->used = 1;
	w->canvas_window = 1;
	w->pid = current_proc()->pid;
	w->owner_cr3 = base;
	w->back = back;
	w->back_pages = pages;
	w->canvas.seq = 1;
	w->canvas.drawn = 0;
	w->pending_back = 0;
	w->pending_pages = 0;
	w->resize_pending = 0;
	w->canvas_dmg_x = w->canvas_dmg_y = 0;
	w->canvas_dmg_w = w->canvas_dmg_h = 0;
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

	out->fb = (void *)USER_WIN_BASE;
	out->width = cw;
	out->height = ch;
	out->id = i;
	return 0;
}

int wm_server_win_present(const struct wm_win_present *in)
{
	int idx = wm_window_for_pid(current_proc()->pid);
	struct window *w;
	const unsigned short *src;
	unsigned int x = 0, y = 0, rw, rh, row;

	if (idx < 0)
		return -1;
	w = &windows[idx];
	if (!w->canvas_window || !w->back || !w->canvas.px ||
	    w->resize_pending)
		return -1;
	rw = w->canvas.w;
	rh = w->canvas.h;
	if (in && in->w && in->h) {
		x = in->x;
		y = in->y;
		rw = in->w;
		rh = in->h;
		if (x >= w->canvas.w || y >= w->canvas.h)
			return -1;
		if (rw > w->canvas.w - x)
			rw = w->canvas.w - x;
		if (rh > w->canvas.h - y)
			rh = w->canvas.h - y;
	}

	src = (const unsigned short *)USER_WIN_BASE;
	for (row = 0; row < rh; row++) {
		const unsigned short *s = src + (unsigned long)(y + row) *
					       w->canvas.w + x;
		unsigned short *d = w->canvas.px +
				    (unsigned long)(y + row) * w->canvas.w + x;
		unsigned int n = rw;

		__asm__ volatile ("cld; rep movsw"
				  : "+D"(d), "+c"(n), "+S"(s)
				  :
				  : "memory");
	}

	if (!w->canvas_dmg_w || !w->canvas_dmg_h) {
		w->canvas_dmg_x = x;
		w->canvas_dmg_y = y;
		w->canvas_dmg_w = rw;
		w->canvas_dmg_h = rh;
	} else {
		unsigned int x1 = w->canvas_dmg_x + w->canvas_dmg_w;
		unsigned int y1 = w->canvas_dmg_y + w->canvas_dmg_h;

		if (x < w->canvas_dmg_x)
			w->canvas_dmg_x = x;
		if (y < w->canvas_dmg_y)
			w->canvas_dmg_y = y;
		if (x + rw > x1)
			x1 = x + rw;
		if (y + rh > y1)
			y1 = y + rh;
		w->canvas_dmg_w = x1 - w->canvas_dmg_x;
		w->canvas_dmg_h = y1 - w->canvas_dmg_y;
	}
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
	out->fb = (void *)USER_WIN_BASE;
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
		paging_release_user_range(w->owner_cr3, USER_WIN_BASE,
					  old_pages * PAGE_SIZE, 0);
	for (i = 0; i < w->pending_pages; i++) {
		if (!paging_map_page(current_proc()->cr3,
				     USER_WIN_BASE + i * PAGE_SIZE,
				     (unsigned int)w->pending_back + i * PAGE_SIZE,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			/* Put the old mapping back; this is a last-resort path. */
			paging_release_user_range(current_proc()->cr3,
						  USER_WIN_BASE,
						  i * PAGE_SIZE, 0);
			for (i = 0; i < old_pages; i++)
				paging_map_page(current_proc()->cr3,
					USER_WIN_BASE + i * PAGE_SIZE,
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
		invlpg_va(USER_WIN_BASE + i * PAGE_SIZE);
	}
	for (i = 0; i < old_pages; i++)
		mm_free_page((unsigned int)w->back + i * PAGE_SIZE);
	w->back = w->pending_back;
	w->back_pages = w->pending_pages;
	w->pending_back = 0;
	w->pending_pages = 0;
	w->resize_pending = 0;
	w->canvas_dmg_w = w->canvas_dmg_h = 0;
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
	w->canvas_dmg_x = w->canvas_dmg_y = 0;
	w->canvas_dmg_w = w->canvas_dmg_h = 0;
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
	wm_frame_active = 0;
	wm_boot_logged = 0;
	wm_last_seen = get_ticks_ms();
	mouse_set_auto_draw(1);
	wm_full_redraw = 1;
	/* The WM owns the display from its first frame on; stop the boot
	 * console before a later boot line can draw over the desktop. */
	bootcon_set_enabled(0);

	out->width = s->w;
	out->height = s->h;
	out->pitch = s->pitch;
	out->bpp = s->bpp;
	out->bytes = bytes;
	out->fb = (void *)WM_SHADOW_VA;
	serial_puts(SERIAL_COM1, "wm: ring-3 WM attached\n");
	return 0;
}

int wm_server_info(unsigned int index, struct wm_window_info *out)
{
	struct window *w;
	struct gfx_canvas *g;
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

	if (!w->canvas_window)
		return 1;
	g = &w->canvas;
	out->content_w = g->w;
	out->content_h = g->h;
	out->dirty = (g->seq != g->drawn);
	out->dirty_seq = g->seq;
	if (out->dirty && w->canvas_dmg_w && w->canvas_dmg_h) {
		out->dmg_x = w->canvas_dmg_x;
		out->dmg_y = w->canvas_dmg_y;
		out->dmg_w = w->canvas_dmg_w;
		out->dmg_h = w->canvas_dmg_h;
	} else if (out->dirty) {
		out->dmg_w = g->w;
		out->dmg_h = g->h;
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
	if (!w->used || !w->canvas_window)
		return -1;
	g = &w->canvas;
	src = (const void *)g->px;
	if (!src)
		return -1;
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

	if (!wm_server_is_wm() || index >= MAX_WINDOWS)
		return -1;
	w = &windows[index];
	if (!w->used || !w->canvas_window)
		return -1;
	if (seq == w->canvas.seq) {
		w->canvas.drawn = seq;
		w->canvas_dmg_w = w->canvas_dmg_h = 0;
	}
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

int wm_spawn_terminal(void)
{
	struct vfs_file f;
	struct vfs_mount *m = 0;
	unsigned int flags;
	int pid;

	if (vfs_load_file("/ramfs/bin/term", &f, &m) != 0) {
		serial_puts(SERIAL_COM1, "wm: /ramfs/bin/term not found\n");
		return -1;
	}
	flags = irq_save_disable();
	pid = process_create_user_tty("term", f.data, f.size, "", "/", 0);
	irq_restore(flags);
	vfs_release_file(m, &f);
	if (pid <= 0) {
		serial_puts(SERIAL_COM1, "wm: cannot start ring-3 term\n");
		return -1;
	}
	process_set_weight((unsigned int)pid, 12);
	wm_request_redraw();
	return 0;
}

int wm_server_create_term(void)
{
	if (!wm_server_is_wm())
		return -1;
	return wm_spawn_terminal();
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
	out->boot_ready = wm_boot_ready ? 1 : 0;
	out->kexec_busy = kexec_busy ? 1 : 0;
	out->kexec_progress = kexec_progress;
	out->kexec_total = kexec_total;
	wm_full_redraw = 0;
	return 0;
}

void wm_server_boot_ready(void)
{
	wm_boot_ready = 1;
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

	if (vfs_load_file("/ramfs/bin/wm", &f, &m) != 0) {
		if (!wm_boot_logged) {
			wm_boot_logged = 1;
			serial_puts(SERIAL_COM1, "wm: /ramfs/bin/wm not found\n");
			bootcon_puts("wm: /ramfs/bin/wm not found\n");
		}
		return;
	}

	flags = irq_save_disable();
	pid = process_create_user("wm", f.data, f.size, "", "/");
	if (pid > 0)
		wm_expected_pid = (unsigned int)pid;
	irq_restore(flags);
	vfs_release_file(m, &f);
	if (pid <= 0) {
		if (!wm_boot_logged) {
			wm_boot_logged = 1;
			serial_puts(SERIAL_COM1, "wm: cannot load /ramfs/bin/wm\n");
			bootcon_puts("wm: cannot load /ramfs/bin/wm\n");
		}
		return;
	}
	process_set_weight((unsigned int)pid, 16);
	wm_boot_logged = 0;
	serial_puts(SERIAL_COM1, "wm: ring-3 WM started\n");
}

void wm_server_supervise(void)
{
	unsigned int i, now = get_ticks_ms();

	/* Watchdog: a WM that never attaches, or stops polling for several
	 * seconds, is treated like a crash so a wedged UI can recover. */
	if (wm_expected_pid && process_is_alive(wm_expected_pid) &&
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

	for (i = 0; i < MAX_WINDOWS; i++) {
		windows[i].used = 0;
		windows[i].mapped_kind = WM_KIND_NONE;
		windows[i].mapped_gen = 0;
		windows[i].mapped_pages = 0;
		windows[i].mapped_src = 0;
	}
	wm_full_redraw = 1;
	wm_boot_ready = 0;
	wm_boot_logged = 0;
	wm_ptr_buttons = 0;
	wm_ptr_capture = -1;
	wm_expected_pid = 0;
	wm_pid = 0;
	wm_cr3 = 0;
	wm_attached = 0;
	wm_frame_active = 0;
	wm_first_frame = 0;
	/* The ring-3 WM spawns the first terminal once wm_server_boot_ready()
	 * says the storage scan is done. */
}

static int wm_point_in_rect(unsigned int px, unsigned int py,
	unsigned int rx, unsigned int ry, unsigned int rw, unsigned int rh)
{
	return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
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
	wait_wake_all(&w->key_wq);
	if (w->canvas_window) {
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

/* Repaint the desktop rectangle (x, y, w, h): background, then every
   intersecting window back-to-front (focused last), then mark it dirty.  The
   caller handles the mouse cursor.  The rect must lie below the system bar
   (windows are clamped to y >= WM_BAR_H, so the bar is never overlapped and
   needs no clip variant). */

/* Move a window in the shadow buffer: copy its rect, redraw the exposed
   sliver, keep the cursor on top, and flush the swept band immediately so a
   drag step is always one consistent frame (a budgeted flush would copy only
   the top rows of the band per iteration and the window would appear to
   stretch while the mouse keeps moving). */


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
		if (fw->canvas_window) {
			unsigned int next = (fw->key_head + 1) % 16;

			if (next != fw->key_tail) {
				fw->keys[fw->key_head] = k;
				fw->key_head = next;
			}
			wait_wake_one(&fw->key_wq);
		}
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

