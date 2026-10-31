/* Fatal exception screen: see panic.h. */

#include "panic.h"
#include "font_8x8.h"
#include "process.h"
#include "serial.h"
#include "timer.h"
#include "vga.h"

#define LINE_H      10
#define PANIC_LINES 12
#define RAM_LIMIT   0x08000000u   /* MM_MAX_PHYS: identity-mapped RAM */
#define KERNEL_BASE 0x7E00u       /* kernel load address (kernel.ld) */

static unsigned int fb_w, fb_h, fb_pitch, fb_bpp;
static volatile unsigned char *fb_lfb;
static int fb_ok;
static unsigned int panic_line;

static const char hx[] = "0123456789ABCDEF";

extern unsigned char __bss_end[];

/* 8 GPRs from pusha + vector/err/eip/cs/eflags/esp/ss. */
_Static_assert(sizeof(struct exc_state) == 15 * sizeof(unsigned int),
	       "struct exc_state must match the timer.S stub");

/* --- direct LFB drawing (works with no WM/shadow) ----------------------- */

static void cache_fb(void)
{
	fb_w = vga_fb_width();
	fb_h = vga_fb_height();
	fb_pitch = vga_fb_pitch();
	fb_bpp = vga_fb_bpp();
	fb_lfb = (volatile unsigned char *)(unsigned long)vga_fb_lfb_addr();
	fb_ok = (fb_w > 0 && fb_h > 0 && fb_pitch > 0 && fb_lfb &&
		 (fb_bpp == 16 || fb_bpp == 24 || fb_bpp == 32));
}

static void px(unsigned int x, unsigned int y,
	       unsigned int r, unsigned int g, unsigned int b)
{
	volatile unsigned char *p;

	if (!fb_ok || x >= fb_w || y >= fb_h)
		return;
	p = fb_lfb + (unsigned long)y * fb_pitch + (unsigned long)x * (fb_bpp / 8);
	if (fb_bpp == 16) {
		unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		p[0] = (unsigned char)(v & 0xFF);
		p[1] = (unsigned char)(v >> 8);
	} else {
		p[0] = (unsigned char)b;
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)r;
		if (fb_bpp == 32)
			p[3] = 0;
	}
}

static void rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		 unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			px(x + i, y + j, r, g, b);
}

static void draw_ch(unsigned int x, unsigned int y, char c, unsigned int sc,
		    unsigned int r, unsigned int g, unsigned int b)
{
	const unsigned char *gl = &font_8x8[(unsigned char)c * 8];
	unsigned int row, col, i, j;

	for (row = 0; row < 8; row++) {
		unsigned char bits = gl[row];
		for (col = 0; col < 8; col++) {
			if (bits & (0x80 >> col)) {
				for (i = 0; i < sc; i++)
					for (j = 0; j < sc; j++)
						px(x + col * sc + i,
						   y + row * sc + j, r, g, b);
			}
		}
	}
}

static void draw_str(unsigned int x, unsigned int y, unsigned int sc,
		     const char *s, unsigned int r, unsigned int g, unsigned int b)
{
	while (*s) {
		draw_ch(x, y, *s, sc, r, g, b);
		x += 8 * sc;
		s++;
	}
}

/* --- small freestanding formatters ------------------------------------- */

static unsigned int app_str(char *buf, unsigned int i, const char *s)
{
	while (*s)
		buf[i++] = *s++;
	return i;
}

static unsigned int app_dec(char *buf, unsigned int i, unsigned int v)
{
	char tmp[11];
	unsigned int n = 0, j;

	if (v == 0) {
		buf[i++] = '0';
		return i;
	}
	while (v) {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	for (j = n; j > 0; j--)
		buf[i++] = tmp[j - 1];
	return i;
}

static unsigned int app_hex(char *buf, unsigned int i, unsigned int v,
			    unsigned int digits)
{
	unsigned int d;

	for (d = 0; d < digits; d++)
		buf[i++] = hx[(v >> (4 * (digits - 1 - d))) & 0xF];
	return i;
}

static unsigned int app_ptr(char *buf, unsigned int i, unsigned int v)
{
	i = app_str(buf, i, "0x");
	return app_hex(buf, i, v, 8);
}

/* One report line: the screen (until the block is full) and COM1. */
static void emit(const char *s)
{
	if (fb_ok && panic_line < PANIC_LINES) {
		unsigned int y = 26 + panic_line * LINE_H;

		draw_str(4, y, 1, s, 255, 235, 190);
	}
	panic_line++;
	serial_puts(SERIAL_COM1, s);
	serial_putc(SERIAL_COM1, '\n');
}

/* --- the report --------------------------------------------------------- */

void panic_report(const struct exc_state *st, const char *name)
{
	char buf[128];
	unsigned int i, n, k;
	unsigned int cr0, cr2, cr3, cr4;
	unsigned int esp, page_top;
	unsigned int code_n = 0;
	unsigned char code[16];
	unsigned int cand_val[12], cand_off[12];
	unsigned int cand_n = 0;
	int stack_ok, user = (st->cs & 3) == 3;

	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	__asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
	__asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

	/* Fault-time stack pointer: the CPU also pushes SS:ESP on a privilege
	 * change; for a ring-0 fault it is just above the EFLAGS slot. */
	esp = user ? st->esp : (unsigned int)&st->eflags + 4;
	stack_ok = (esp >= 0x1000u && esp < RAM_LIMIT);
	page_top = stack_ok ? (esp | 0xFFFu) + 1 : esp;

	/* The faulting instruction bytes (only from mapped RAM, and never
	 * across a page boundary, so a bad EIP cannot fault the handler). */
	if (st->eip < RAM_LIMIT) {
		unsigned int eip_top = (st->eip | 0xFFFu) + 1;

		while (code_n < sizeof(code) && st->eip + code_n < eip_top) {
			code[code_n] = *(volatile unsigned char *)(st->eip + code_n);
			code_n++;
		}
	}

	/* Return-address candidates: scan the faulting stack page for dwords
	 * that point into the kernel image.  There is no EBP chain to walk
	 * (-O2 omits frame pointers), so these are best-effort hints. */
	if (stack_ok) {
		for (n = esp; n + 4 <= page_top && cand_n < 12; n += 4) {
			unsigned int v = *(volatile unsigned int *)n;

			if (v >= KERNEL_BASE && v < (unsigned int)__bss_end) {
				cand_val[cand_n] = v;
				cand_off[cand_n] = n - esp;
				cand_n++;
			}
		}
	}

	cache_fb();
	if (fb_ok) {
		rect(0, 0, fb_w, 22, 160, 0, 0);
		draw_str(4, 4, 2, "*** KERNEL EXCEPTION ***", 255, 255, 255);
		rect(0, 22, fb_w, 6 + PANIC_LINES * LINE_H, 24, 0, 0);
	}
	panic_line = 0;
	serial_puts(SERIAL_COM1, "\n*** KERNEL EXCEPTION ***\n");

	/* Vector, error code, and the #PF error-code bits. */
	i = 0;
	i = app_str(buf, i, name);
	i = app_str(buf, i, "  vec ");
	i = app_dec(buf, i, st->vector);
	i = app_str(buf, i, "  err 0x");
	i = app_hex(buf, i, st->err, 8);
	if (st->vector == 14) {
		i = app_str(buf, i, "  P=");
		buf[i++] = (char)((st->err & 0x01) ? '1' : '0');
		i = app_str(buf, i, " W=");
		buf[i++] = (char)((st->err & 0x02) ? '1' : '0');
		i = app_str(buf, i, " U=");
		buf[i++] = (char)((st->err & 0x04) ? '1' : '0');
		i = app_str(buf, i, " RSVD=");
		buf[i++] = (char)((st->err & 0x08) ? '1' : '0');
		i = app_str(buf, i, " ID=");
		buf[i++] = (char)((st->err & 0x10) ? '1' : '0');
	}
	buf[i] = '\0';
	emit(buf);

	/* Faulting instruction pointer and flags. */
	i = 0;
	i = app_str(buf, i, "EIP ");
	i = app_ptr(buf, i, st->eip);
	i = app_str(buf, i, "  CS ");
	i = app_str(buf, i, "0x");
	i = app_hex(buf, i, st->cs, 4);
	i = app_str(buf, i, user ? " ring 3" : " ring 0");
	i = app_str(buf, i, "  EFLAGS 0x");
	i = app_hex(buf, i, st->eflags, 8);
	if (st->eflags & 0x200u)
		i = app_str(buf, i, " IF");
	if (st->eflags & 0x20000u)
		i = app_str(buf, i, " VM");
	buf[i] = '\0';
	emit(buf);

	/* General registers (two lines of four). */
	i = 0;
	i = app_str(buf, i, "EAX "); i = app_ptr(buf, i, st->eax);
	i = app_str(buf, i, "  EBX "); i = app_ptr(buf, i, st->ebx);
	i = app_str(buf, i, "  ECX "); i = app_ptr(buf, i, st->ecx);
	i = app_str(buf, i, "  EDX "); i = app_ptr(buf, i, st->edx);
	buf[i] = '\0';
	emit(buf);

	i = 0;
	i = app_str(buf, i, "ESI "); i = app_ptr(buf, i, st->esi);
	i = app_str(buf, i, "  EDI "); i = app_ptr(buf, i, st->edi);
	i = app_str(buf, i, "  EBP "); i = app_ptr(buf, i, st->ebp);
	i = app_str(buf, i, "  ESP "); i = app_ptr(buf, i, esp);
	buf[i] = '\0';
	emit(buf);

	/* Control registers. */
	i = 0;
	i = app_str(buf, i, "CR0 "); i = app_ptr(buf, i, cr0);
	i = app_str(buf, i, "  CR2 "); i = app_ptr(buf, i, cr2);
	i = app_str(buf, i, "  CR3 "); i = app_ptr(buf, i, cr3);
	i = app_str(buf, i, "  CR4 "); i = app_ptr(buf, i, cr4);
	buf[i] = '\0';
	emit(buf);

	/* Current process and uptime. */
	i = 0;
	i = app_str(buf, i, "pid ");
	i = app_dec(buf, i, current_pid);
	if (procs) {
		i = app_str(buf, i, " (");
		for (n = 0; n < PROC_NAME_LEN && procs[current_pid].name[n]; n++)
			buf[i++] = procs[current_pid].name[n];
		buf[i++] = ')';
	}
	i = app_str(buf, i, "  ticks ");
	i = app_dec(buf, i, get_ticks_ms());
	buf[i] = '\0';
	emit(buf);

	/* Kernel stack bounds: an ESP below the base means stack overflow. */
	if (procs) {
		i = 0;
		i = app_str(buf, i, "kstack base ");
		i = app_ptr(buf, i, procs[current_pid].stack_base);
		i = app_str(buf, i, "  top ");
		i = app_ptr(buf, i, procs[current_pid].kstack_top);
		buf[i] = '\0';
		emit(buf);
	}

	/* Bytes at EIP: the faulting instruction (raw, for a disassembler). */
	i = 0;
	i = app_str(buf, i, "code");
	for (n = 0; n < code_n; n++) {
		buf[i++] = ' ';
		buf[i++] = hx[code[n] >> 4];
		buf[i++] = hx[code[n] & 0xF];
	}
	if (code_n == 0)
		i = app_str(buf, i, " (unmapped)");
	buf[i] = '\0';
	emit(buf);

	/* A few dwords at the faulting stack pointer. */
	if (stack_ok) {
		for (k = 0; k < 2; k++) {
			unsigned int p = esp + k * 16;

			i = 0;
			i = app_str(buf, i, "stk ");
			i = app_ptr(buf, i, p);
			i = app_str(buf, i, ":");
			for (n = 0; n < 4; n++) {
				unsigned int a = p + n * 4;

				if (a + 4 > page_top)
					break;
				i = app_str(buf, i, " ");
				i = app_hex(buf, i, *(volatile unsigned int *)a, 8);
			}
			buf[i] = '\0';
			emit(buf);
		}
	} else {
		i = 0;
		i = app_str(buf, i, "stk ");
		i = app_ptr(buf, i, esp);
		i = app_str(buf, i, " (not mapped)");
		buf[i] = '\0';
		emit(buf);
	}

	/* Return-address candidates found on the stack. */
	i = 0;
	i = app_str(buf, i, "ret");
	for (n = 0; n < cand_n && n < 5; n++) {
		buf[i++] = ' ';
		i = app_ptr(buf, i, cand_val[n]);
	}
	if (cand_n == 0)
		i = app_str(buf, i, " none");
	buf[i] = '\0';
	emit(buf);

	/* Serial gets the full stack and every candidate, each with its offset
	 * from the faulting ESP, so a crash log can be read without the screen. */
	if (stack_ok) {
		for (n = 0; n < 32; n++) {
			unsigned int p = esp + n * 4;

			if (p + 4 > page_top)
				break;
			i = 0;
			i = app_str(buf, i, "stk +");
			i = app_hex(buf, i, n * 4, 3);
			i = app_str(buf, i, " @");
			i = app_hex(buf, i, p, 8);
			i = app_str(buf, i, " = ");
			i = app_hex(buf, i, *(volatile unsigned int *)p, 8);
			buf[i] = '\0';
			serial_puts(SERIAL_COM1, buf);
			serial_putc(SERIAL_COM1, '\n');
		}
	}
	for (n = 0; n < cand_n; n++) {
		i = 0;
		i = app_str(buf, i, "ret +");
		i = app_hex(buf, i, cand_off[n], 3);
		i = app_str(buf, i, " ");
		i = app_ptr(buf, i, cand_val[n]);
		buf[i] = '\0';
		serial_puts(SERIAL_COM1, buf);
		serial_putc(SERIAL_COM1, '\n');
	}
}
