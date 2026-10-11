/* On-screen kernel debug output (no serial console needed).
 *
 * Draws straight into the linear framebuffer, bypassing the shadow buffer
 * and the window manager, so it still works when the WM is wedged.  Used on
 * real hardware to debug e.g. keyboard/IRQ problems:
 *
 *   - kdbg_status():  two counter lines in the bottom-left corner
 *   - kdbg_pit_tick(): 0.5s heartbeat square (bottom-right) + periodic
 *                     status refresh, called from the PIT IRQ
 *   - kdbg_panic():   red banner with exception info
 */
#ifndef KDBG_H
#define KDBG_H

void kdbg_init(void);
void kdbg_status(void);
void kdbg_pit_tick(void);
void kdbg_panic(const char *name, unsigned int eip, unsigned int cr2);

/* Extra debug lines drawn above the status block (row 0 = lowest, at
   fb_h-31).  Text is copied, so callers may pass stack buffers.  Safe to
   call before kdbg_init(): the text is stored and drawn once the
   framebuffer is up. */
void kdbg_info(unsigned int row, const char *s);

/* Allocate the next free info row (boot subsystems claim rows in init
   order).  Returns -1 when every row is already taken. */
int kdbg_info_alloc(void);

/* Counters shared by the keyboard IRQ, the WM and the terminal. */
extern volatile unsigned int kdbg_irq_count;   /* IRQ1 entries */
extern volatile unsigned int kdbg_irq_bytes;   /* scancode bytes read */
extern volatile unsigned int kdbg_irq_other;   /* AUX/empty reads */
extern volatile unsigned int kdbg_last_sc;     /* last scancode read */
extern volatile unsigned int kdbg_irq_masked;  /* storm guard tripped */
extern volatile unsigned int kdbg_wm_count;    /* keys handled by the WM */
extern volatile unsigned int kdbg_wm_last;
extern volatile unsigned int kdbg_term_count;  /* keys handled by terminal */
extern volatile unsigned int kdbg_term_last;

#endif
