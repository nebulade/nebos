/*
 * Context-switch PIT IRQ stub.
 *
 * On entry the CPU has already pushed EFLAGS, CS, EIP onto the
 * current process's stack.  We save the rest, bump the tick counter,
 * and -- when the timeslice expires -- call schedule() and swap ESP
 * to the next process.
 *
 * Shadow buffer flush: every ~33 ticks (30Hz), set pending flag.
 * Main loop checks dirty rows and copies only what changed.
 *
 * Serial poll: every tick, check UART for received data and push
 * to ring buffer so no bytes are lost during preemption.
 */
.code32
.text

.extern ticks
.extern current_pid
.extern procs
.extern schedule
.extern vga_shadow_flush_pending
.extern serial_poll
.extern kdbg_pit_tick
.extern tss

.include "irqret.inc"

/* struct process offsets (must match process.h / process.c) */
.equ PCB_ESP,   0
.equ PCB_CR3,   32
.equ PCB_KSTACK_TOP, 36
.equ PCB_SIZE,  44      /* sizeof(struct process): 4+4+4+12+4+4+4+4+4 = 44 */

.equ TIMESLICE, 20
.equ FLUSH_INTERVAL, 33 /* 1000Hz PIT / 30Hz flush ≈ 33 ticks */

.globl pit_irq_stub
pit_irq_stub:
	pusha

	/* Load the kernel data segment.  The PIT can fire while a ring-3 process
	 * is running, and in that case the CPU has switched CS/SS but left DS
	 * null (IRET reset it when it returned to ring-3); the data accesses
	 * below (ticks, counters, procs, tss) go through DS and would #GP. */
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs

	/* ticks++ and send EOI -- always, even when we don't switch. */
	incl  ticks
	movb  $0x20, %al
	outb  %al, $0x20

	/* Poll UART for received serial data (prevents FIFO overflow). */
	call  serial_poll

	/* On-screen debug heartbeat / status refresh (no serial console on
	 * some targets; this keeps a live indicator on the framebuffer). */
	call  kdbg_pit_tick

	/* Shadow buffer flush at 30Hz: set pending flag, main loop does the copy. */
	decl  flush_counter
	jnz   .no_flush
	movl  $FLUSH_INTERVAL, flush_counter
	movl  $1, vga_shadow_flush_pending
.no_flush:

	/* Decrement timeslice counter; switch only when it reaches 0. */
	decl  slice_counter
	jnz   .no_switch

	/* Reset counter for next timeslice. */
	movl  $TIMESLICE, slice_counter

	/* Save current ESP into procs[current_pid].esp (procs is a pointer). */
	movl  current_pid, %eax
	imull $PCB_SIZE, %eax
	addl  procs, %eax
	movl  %esp, PCB_ESP(%eax)

	/* Let the C scheduler pick the next process. */
	call  schedule

	/* Load new ESP, CR3, and TSS esp0 from procs[current_pid]. */
	movl  current_pid, %eax
	imull $PCB_SIZE, %eax
	addl  procs, %eax
	movl  PCB_ESP(%eax), %esp
	movl  PCB_CR3(%eax), %ebx
	movl  %ebx, %cr3
	movl  PCB_KSTACK_TOP(%eax), %ebx
	movl  %ebx, tss+4           /* TSS.esp0 = this process's ring-0 stack top */

.no_switch:
	/* Both paths (return to the interrupted process, or to one just switched
	 * in) must leave a usable data segment selector for the context being
	 * resumed; ring-3 needs 0x23 (see irqret.inc). */
	IRQ_RETURN_DS 36

	popa
	iret

.data
.align 4
slice_counter:
	.long TIMESLICE
flush_counter:
	.long FLUSH_INTERVAL
