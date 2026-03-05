/* PIT IRQ0 stub: save state, call C handler, restore, iret. */
.code32
.text

.globl pit_irq_stub
pit_irq_stub:
	pusha
	call  pit_irq
	popa
	iret
