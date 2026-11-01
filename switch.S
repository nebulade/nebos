/*
 * Context-switch PIT IRQ stub.
 *
 * On entry the CPU has already pushed EFLAGS, CS, EIP onto the current
 * process's stack.  We save the rest, bump the tick counter, and -- when
 * sched_tick_bsp() says the timeslice expired -- call schedule() and swap
 * ESP to the next process.
 *
 * The only process facts this file knows are:
 *   - %gs:0 is struct cpu.current
 *   - offset 0 of struct process is esp (saved/restored here)
 * CR3 and TSS.esp0 are switched by schedule() in C.
 *
 * Serial poll: every tick, check UART for received data and push to the ring
 * buffer so no bytes are lost during preemption.
 */
.code32
.text

.extern ticks
.extern schedule
.extern sched_tick_bsp
.extern serial_poll

.include "irqret.inc"

.globl pit_irq_stub
pit_irq_stub:
	pusha

	/* Load the kernel selectors.  The PIT can fire while a ring-3 process
	 * is running; DS would then be null after the IRET that entered ring 3,
	 * and the accesses below would #GP.  GS is the per-CPU data selector. */
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  $0x28, %ax
	movw  %ax, %gs

	/* ticks++ and send EOI -- always, even when we don't switch. */
	incl  ticks
	movb  $0x20, %al
	outb  %al, $0x20

	/* Poll UART for received serial data (prevents FIFO overflow). */
	call  serial_poll

	/* Account the tick and decide whether the timeslice expired. */
	call  sched_tick_bsp
	testl %eax, %eax
	jz    .no_switch

	/* Save the interrupted ESP into current->esp. */
	movl  %gs:0, %eax
	movl  %esp, (%eax)

	/* Let the C scheduler pick the next process and switch CR3/TSS. */
	call  schedule

	/* Load the new process's ESP. */
	movl  %gs:0, %eax
	movl  (%eax), %esp

.no_switch:
	/* Both paths must leave a usable data selector for the context being
	 * resumed; ring-3 needs 0x23, ring-0 uses 0x10 + GS 0x28. */
	IRQ_RETURN_DS 36

	popa
	iret

/*
 * Voluntary reschedule gate: int 0x81.  Entered from ring 0 only, with the
 * CPU having pushed EFLAGS/CS/EIP exactly like the PIT path.  No EOI and no
 * tick accounting; just save ESP, pick a new process and restore its frame.
 */
.globl resched_stub
resched_stub:
	pusha
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  $0x28, %ax
	movw  %ax, %gs

	movl  %gs:0, %eax
	movl  %esp, (%eax)
	call  schedule
	movl  %gs:0, %eax
	movl  (%eax), %esp

	IRQ_RETURN_DS 36
	popa
	iret
