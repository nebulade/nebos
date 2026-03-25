#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_poll_update(void);

int mouse_is_present(void);
int mouse_streaming_enabled(void);
unsigned int mouse_device_id(void);
int mouse_cursor_visible(void);

#endif
