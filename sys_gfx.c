/* Window graphics sessions: one double-buffered canvas per window, mapped
 * into the owning ring-3 process.  The process draws into the mapped back
 * buffer; SYS_GFX_PRESENT copies it into the front buffer, which is the only
 * thing the WM ever composites - so a half-drawn frame can never reach the
 * screen (the copy runs with interrupts off).  Sessions are keyed by the
 * owner's pid and by the terminal screen (the screen pointer is what a
 * spawned command inherits, so it identifies the window without any new
 * plumbing in process.c). */

#include "sys_gfx.h"
#include "mm.h"
#include "paging.h"
#include "process.h"
#include "wm.h"

#define GFX_MAX_SESSIONS MAX_WINDOWS
#define GFX_USER_BASE    USER_GFX_BASE   /* canvas mapping for the owner */
#define GFX_KEY_RING     16

struct gfx_session {
	int used;
	int dead;             /* owner gone: free on the next WM collect */
	unsigned int pid;
	unsigned int cr3;     /* owner's page directory (to unmap on END) */
	struct term *term;
	struct gfx_canvas c;  /* front buffer: what the WM composites */
	unsigned short *back; /* back buffer: mapped to the owner */
	unsigned int back_pages;
	int keys[GFX_KEY_RING];
	unsigned int key_head, key_tail;
};

static struct gfx_session sessions[GFX_MAX_SESSIONS];

static inline void invlpg_va(unsigned int va)
{
	__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static struct gfx_session *by_pid(unsigned int pid)
{
	unsigned int i;

	for (i = 0; i < GFX_MAX_SESSIONS; i++)
		if (sessions[i].used && !sessions[i].dead && sessions[i].pid == pid)
			return &sessions[i];
	return 0;
}

static struct gfx_session *by_screen(struct text_screen *s)
{
	unsigned int i;

	for (i = 0; i < GFX_MAX_SESSIONS; i++)
		if (sessions[i].used && !sessions[i].dead &&
		    &sessions[i].term->screen == s)
			return &sessions[i];
	return 0;
}

static void free_back(struct gfx_session *s)
{
	unsigned int i;

	if (!s->back)
		return;
	for (i = 0; i < s->back_pages; i++)
		mm_free_page((unsigned int)s->back + i * PAGE_SIZE);
	s->back = 0;
	s->back_pages = 0;
}

static void unmap_all(struct gfx_session *s)
{
	unsigned int i;

	if (!s->cr3 || !s->back)
		return;
	for (i = 0; i < s->back_pages; i++)
		paging_unmap_page(s->cr3, GFX_USER_BASE + i * PAGE_SIZE);
	s->cr3 = 0;
}

int sys_gfx_begin(struct sys_gfx_info *info)
{
	struct gfx_session *s = 0;
	struct term *t;
	unsigned int w, h, i, pages;

	if (!info)
		return -1;
	if (by_pid(procs[current_pid].pid))
		return -1;                       /* one session per process */
	t = wm_term_for_screen(procs[current_pid].screen);
	if (!t)
		return -1;                       /* no window (not spawned by a terminal) */
	if (by_screen(&t->screen))
		return -1;                       /* one gfx command per window */

	for (i = 0; i < GFX_MAX_SESSIONS; i++)
		if (!sessions[i].used) {
			s = &sessions[i];
			break;
		}
	if (!s)
		return -1;
	if (wm_content_size(t, &w, &h) != 0)
		return -1;
	if (gfx_alloc(&s->c, w, h) != 0)
		return -1;

	pages = (w * h * 2 + PAGE_SIZE - 1) / PAGE_SIZE;
	s->back = (unsigned short *)mm_alloc_pages(pages);
	if (!s->back) {
		gfx_free(&s->c);
		return -1;
	}
	s->back_pages = pages;
	for (i = 0; i < w * h; i++)     /* fresh memory is not zeroed */
		s->back[i] = 0;

	for (i = 0; i < pages; i++) {
		if (!paging_map_page(procs[current_pid].cr3,
				     GFX_USER_BASE + i * PAGE_SIZE,
				     (unsigned int)s->back + i * PAGE_SIZE,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			while (i--)
				paging_unmap_page(procs[current_pid].cr3,
						  GFX_USER_BASE + i * PAGE_SIZE);
			free_back(s);
			gfx_free(&s->c);
			return -1;
		}
		invlpg_va(GFX_USER_BASE + i * PAGE_SIZE);
	}

	s->used = 1;
	s->dead = 0;
	s->pid = procs[current_pid].pid;
	s->cr3 = procs[current_pid].cr3;
	s->term = t;
	s->key_head = s->key_tail = 0;

	wm_content_changed(&t->screen);   /* the WM switches to the canvas */

	info->width = w;
	info->height = h;
	info->fb = (void *)GFX_USER_BASE;
	return 0;
}

int sys_gfx_present(void)
{
	struct gfx_session *s = by_pid(procs[current_pid].pid);
	const unsigned short *src;   /* the caller's own view of its canvas */
	unsigned int n, i;

	if (!s)
		return -1;
	n = s->c.w * s->c.h;

	/* The syscall runs on the caller's page directory, so read the canvas
	 * through the caller's mapping: that is exactly the memory the command
	 * drew into and presented. */
	src = (const unsigned short *)GFX_USER_BASE;

	/* Publish: copy the finished frame into the composited front buffer.
	 * The syscall runs with interrupts off, so the WM can never observe (or
	 * blit) a half-copied frame. */
	for (i = 0; i + 1 < n; i += 2)
		((unsigned int *)s->c.px)[i / 2] = ((const unsigned int *)src)[i / 2];
	if (n & 1)
		s->c.px[n - 1] = src[n - 1];
	s->c.seq++;
	return 0;
}

int sys_gfx_end(void)
{
	struct gfx_session *s = by_pid(procs[current_pid].pid);

	if (!s)
		return -1;
	unmap_all(s);
	/* The front canvas is still mapped into the WM.  Mark the session dead
	 * and let sys_gfx_collect() unmap it from the WM before freeing it. */
	s->dead = 1;
	wm_content_changed(&s->term->screen);
	return 0;
}

int sys_gfx_key_poll(void)
{
	struct gfx_session *s = by_pid(procs[current_pid].pid);
	int k;

	if (!s)
		return wm_server_key_poll();   /* generic canvas window */
	if (s->key_tail == s->key_head)
		return 0;
	k = s->keys[s->key_tail];
	s->key_tail = (s->key_tail + 1) % GFX_KEY_RING;
	return k;
}

/* WM: the canvas to composite for a window's screen, or NULL. */
struct gfx_canvas *sys_gfx_canvas_for(struct text_screen *screen)
{
	struct gfx_session *s = by_screen(screen);

	return s ? &s->c : 0;
}

/* WM: hand a key to the focused window's gfx command.  1 = consumed. */
int sys_gfx_push_key(struct text_screen *screen, int k)
{
	struct gfx_session *s = by_screen(screen);
	unsigned int next;

	if (!s)
		return 0;
	next = (s->key_head + 1) % GFX_KEY_RING;
	if (next != s->key_tail) {
		s->keys[s->key_head] = k;
		s->key_head = next;
	}
	return 1;      /* consumed even when the ring is full (drop, don't leak) */
}

/* WM: the window is closing; stop its gfx command and free its canvas. */
void sys_gfx_kill_for_screen(struct text_screen *screen)
{
	struct gfx_session *s = by_screen(screen);

	if (!s)
		return;
	process_kill(s->pid);
	s->dead = 1;   /* freed by the next sys_gfx_collect() */
	wm_content_changed(screen);
}

/* Reaper hook (schedule(), IRQ context): only flag the session. */
void sys_gfx_reap(unsigned int pid)
{
	unsigned int i;

	for (i = 0; i < GFX_MAX_SESSIONS; i++)
		if (sessions[i].used && !sessions[i].dead && sessions[i].pid == pid)
			sessions[i].dead = 1;
}

/* WM loop: free dead sessions away from the renderer (the reaper can run
 * while the WM is mid-blit, so freeing there would be unsafe). */
void sys_gfx_collect(void)
{
	unsigned int i;
	int freed = 0;

	/* The WM may still be composing the front canvas we are about to free.
	 * It calls back after PRESENT; the idle loop also retries. */
	if (wm_server_frame_active())
		return;

	for (i = 0; i < GFX_MAX_SESSIONS; i++) {
		if (sessions[i].used && sessions[i].dead) {
			struct text_screen *screen = &sessions[i].term->screen;

			/* Drop the WM's read-only mapping before the canvas pages
			 * can be reused.  Only this session's canvas: a newer
			 * session may already be mapped for the same screen. */
			wm_server_unmap_canvas(screen, sessions[i].c.px);
			gfx_free(&sessions[i].c);
			free_back(&sessions[i]);
			sessions[i].used = 0;
			wm_content_changed(screen);
			freed = 1;
		}
	}
	if (freed)
		wm_request_redraw();
}
