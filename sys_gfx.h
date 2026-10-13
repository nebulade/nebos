/* Window graphics service for ring-3 commands (see sys_gfx.c). */
#ifndef SYS_GFX_H
#define SYS_GFX_H

#include "gfx.h"
#include "syscall.h"

struct text_screen;

int  sys_gfx_begin(struct sys_gfx_info *info);
int  sys_gfx_present(void);
int  sys_gfx_end(void);
int  sys_gfx_key_poll(void);

/* WM-facing helpers. */
struct gfx_canvas *sys_gfx_canvas_for(struct text_screen *screen);
int  sys_gfx_push_key(struct text_screen *screen, int k);   /* 1 = consumed */
void sys_gfx_kill_for_screen(struct text_screen *screen);   /* window close */
void sys_gfx_reap(unsigned int pid);                        /* IRQ context */
void sys_gfx_collect(void);                                 /* WM loop */

#endif
