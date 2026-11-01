/* Per-terminal output queues for ring-3 commands.
 *
 * The terminal process owns the display; commands write bytes here and the
 * terminal reads/renders them.  One tty per terminal, inherited by children. */
#ifndef TTY_H
#define TTY_H

#define TTY_MAX 4
#define TTY_BUF 4096

int  tty_create(void);                    /* owner = current process */
void tty_close(int id);
int  tty_write(int id, const char *buf, unsigned int len); /* 0/-1, blocks */
int  tty_write_str(int id, const char *s);
int  tty_read(int id, char *buf, unsigned int max);        /* bytes, 0 if none */
int  tty_valid(int id);
void tty_reap(unsigned int pid);          /* schedule() reaper hook */

#endif
