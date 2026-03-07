/*
 * Context-switch PIT IRQ stub.
 *
 * On entry the CPU has already pushed EFLAGS, CS, EIP onto the
 * current process's stack.  We save the rest, bump the tick counter,
 * and -- when the timeslice expires -- call schedule() and swap ESP
 * to the next process.
 */
.code32
.text

.extern ticks
.extern current_pid
.extern procs
.extern schedule

/* struct process offsets (must match process.h / process.c) */
.equ PCB_ESP,   0
.equ PCB_SIZE,  24      /* sizeof(struct process): 4+4+4+12 = 24 */

.equ TIMESLICE, 20

.globl pit_irq_stub
pit_irq_stub:
	pusha

	/* ticks++ and send EOI -- always, even when we don't switch. */
	incl  ticks
	movb  $0x20, %al
	outb  %al, $0x20

	/* Decrement timeslice counter; switch only when it reaches 0. */
	decl  slice_counter
	jnz   .no_switch

	/* Reset counter for next timeslice. */
	movl  $TIMESLICE, slice_counter

	/* Save current ESP into procs[current_pid].esp */
	movl  current_pid, %eax
	imull $PCB_SIZE, %eax
	leal  procs(%eax), %ebx
	movl  %esp, PCB_ESP(%ebx)

	/* Let the C scheduler pick the next process. */
	call  schedule

	/* Load new ESP from procs[current_pid].esp */
	movl  current_pid, %eax
	imull $PCB_SIZE, %eax
	leal  procs(%eax), %ebx
	movl  PCB_ESP(%ebx), %esp

.no_switch:
	popa
	iret

.data
.align 4
slice_counter:
	.long TIMESLICE
