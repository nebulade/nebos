/* IRQ stubs: save state, call C handler, restore, iret.
   PIT stub (pit_irq_stub) lives in switch.S for context switching. */
.code32
.text

.include "irqret.inc"

.globl kbd_irq_stub
kbd_irq_stub:
	pusha
	/* Reload the kernel data segment: this IRQ can fire while a ring-3
	 * process runs, in which case DS is null (IRET reset it). */
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	call  kbd_irq
	IRQ_RETURN_DS 36
	popa
	iret

.globl fdc_irq_stub
fdc_irq_stub:
	pusha
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	call  fdc_irq
	IRQ_RETURN_DS 36
	popa
	iret

/* Default IRQ handler: send EOI and return.  Catches spurious and
   unexpected hardware interrupts that would otherwise triple-fault. */
.globl default_irq_stub
default_irq_stub:
	pushl %eax
	movb  $0x20, %al
	outb  %al, $0x20
	popl  %eax
	iret

/* CPU exception stubs (vectors 0-31).  The handler never returns; it gets a
 * pointer to a struct exc_state (panic.h): the saved GPRs plus the CPU's
 * exception frame (vector, error code, EIP, CS, EFLAGS, [user ESP, SS]). */

.macro exc_no_err vec
exc_stub_\vec:
	pushl $0               /* the CPU pushes no error code: supply a zero */
	pushl $\vec
	jmp   exc_common
.endm

.macro exc_err vec
exc_stub_\vec:
	pushl $\vec            /* the CPU's error code is already on the stack */
	jmp   exc_common
.endm

exc_no_err 0
exc_no_err 1
exc_no_err 2
exc_no_err 3
exc_no_err 4
exc_no_err 5
exc_no_err 6
exc_no_err 7
exc_err    8
exc_no_err 9
exc_err    10
exc_err    11
exc_err    12
exc_err    13
exc_err    14
exc_no_err 15
exc_no_err 16
exc_err    17
exc_no_err 18
exc_no_err 19
exc_no_err 20
exc_no_err 21
exc_no_err 22
exc_no_err 23
exc_no_err 24
exc_no_err 25
exc_no_err 26
exc_no_err 27
exc_no_err 28
exc_no_err 29
exc_no_err 30
exc_no_err 31

exc_common:
	/* Load the kernel data segment: exceptions only switch CS, and DS may be
	 * null (IRET reset it to null when returning to ring-3), which would make
	 * the C handler's string/data accesses fault. */
	pushl %eax
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	popl  %eax

	/* Layout at %esp must match struct exc_state (panic.h). */
	pusha                  /* GPRs exactly as they were at the fault */
	movl  %esp, %eax
	pushl %eax             /* exc_handler(struct exc_state *st) */
	call  exc_handler
	cli
1:	hlt
	jmp   1b

/* Lookup table so C can iterate over the 32 stubs. */
.section .rodata
.globl exc_stub_table
exc_stub_table:
	.long exc_stub_0,  exc_stub_1,  exc_stub_2,  exc_stub_3
	.long exc_stub_4,  exc_stub_5,  exc_stub_6,  exc_stub_7
	.long exc_stub_8,  exc_stub_9,  exc_stub_10, exc_stub_11
	.long exc_stub_12, exc_stub_13, exc_stub_14, exc_stub_15
	.long exc_stub_16, exc_stub_17, exc_stub_18, exc_stub_19
	.long exc_stub_20, exc_stub_21, exc_stub_22, exc_stub_23
	.long exc_stub_24, exc_stub_25, exc_stub_26, exc_stub_27
	.long exc_stub_28, exc_stub_29, exc_stub_30, exc_stub_31
