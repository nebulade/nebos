/* Ring-3 window-manager syscall service (see wm_abi.h / wm.c). */
#ifndef SYS_WM_H
#define SYS_WM_H

#include "wm_abi.h"

int sys_wm_attach(struct wm_screen_info *out);
int sys_wm_info(unsigned int index, struct wm_window_info *out);
int sys_wm_map(unsigned int index, struct wm_map_info *out);
int sys_wm_ack(unsigned int index, unsigned int seq);
int sys_wm_focus(unsigned int index);
int sys_wm_set_geom(unsigned int index, const struct wm_geom *in);
int sys_wm_create_term(void);
int sys_wm_close(unsigned int index);
int sys_wm_poll(struct wm_status *out);
int sys_wm_begin(void);
int sys_wm_present(const struct wm_present_info *in);
int sys_wm_sync(void);

int sys_win_create(const struct win_create *in, struct win_info *out);
int sys_win_present(void);
int sys_win_close(void);
int sys_win_info(struct win_info *out);
int sys_win_ack_resize(void);
int sys_wm_resize(unsigned int index, const struct wm_size *in);
int sys_win_key_wait(unsigned int timeout_ms);

#endif
