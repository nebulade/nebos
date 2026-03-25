/*
 * VESA real-mode stage: runs at 0x7E00 before protected mode.
 *
 * Called by the boot sector in real mode (CS=0, IP=0x7E00).
 * Walks the VBE mode list from the BIOS, picks the best mode
 * (highest resolution, >= 16 bpp, LFB, fits in VRAM), writes
 * the mode info to 0x600, enables A20, enters PM, jumps to
 * kernel_main.
 *
 * If no VESA mode works, boots in text mode (valid=0 at 0x600).
 */

.code16
.section .text.stage2, "ax", @progbits
.globl _vesa_stage
_vesa_stage:
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es

	/* ---- VBE Controller Info into 0:0x2000 ---- */
	movw  $0x2000, %di
	movw  $0x4256, (%di)	/* "VB" */
	movw  $0x3245, 2(%di)	/* "E2" */
	movw  $0x4F00, %ax
	int   $0x10
	cmpw  $0x004F, %ax
	jnz   vesa_no_vbe

	/* Save VRAM size (offset 0x12: TotalMemory in 64KB blocks). */
	movw  0x12(%di), %ax
	testw %ax, %ax
	jnz   1f
	movw  $0xFFFF, %ax
1:	movw  %ax, vram_64k

	/* Get mode list pointer (offset 0x0E: far pointer seg:off). */
	movw  0x200E, %si	/* offset (buffer at 0x2000) */
	movw  0x2010, %ax	/* segment */
	movw  %ax, %fs		/* FS:SI = mode list */

	/* ---- Walk mode list, pick best mode ---- */
	movw  $0, best_mode
	movl  $0, best_score

walk_modes:
	movw  %fs:(%si), %cx
	cmpw  $0xFFFF, %cx
	jz    walk_done
	addw  $2, %si

	/* Query mode info: VBE 4F01h into 0:0x2200. */
	pushw %si
	pushw %cx
	xorw  %ax, %ax
	movw  %ax, %es
	movw  $0x2200, %di
	movw  $0x4F01, %ax
	int   $0x10
	popw  %cx
	popw  %si
	cmpw  $0x004F, %ax
	jnz   walk_modes

	/* Check: supported (bit 0) + graphics (bit 4) + LFB (bit 7). */
	movw  0x2200, %ax
	andw  $0x0091, %ax
	cmpw  $0x0091, %ax
	jnz   walk_modes

	/* Check: BitsPerPixel >= 16. */
	cmpb  $16, 0x2219
	jb    walk_modes

	/* Check: fits in VRAM. */
	movw  0x2210, %ax
	mulw  0x2214
	testw %ax, %ax
	jz    2f
	incw  %dx
2:	cmpw  vram_64k, %dx
	ja    walk_modes

	/* Score = width * height + bpp. */
	movzwl 0x2212, %eax
	movzwl 0x2214, %edx
	imull  %edx, %eax
	movzbl 0x2219, %edx
	addl   %edx, %eax
	cmpl   best_score, %eax
	jbe    walk_modes

	movl   %eax, best_score
	movw   %cx, best_mode

	/* Save mode info block to 0:0x2400. */
	pushw %si
	pushw %cx
	movw  $0x2200, %si
	movw  $0x2400, %di
	movw  $128, %cx
	cld
	rep movsw
	popw  %cx
	popw  %si
	jmp   walk_modes

walk_done:
	/* Did we find a mode? */
	movw  best_mode, %cx
	testw %cx, %cx
	jz    vesa_no_vbe

	/* ---- Set the mode ---- */
	movw  %cx, %bx
	orw   $0x4000, %bx	/* LFB */
	movw  $0x4F02, %ax
	int   $0x10
	cmpw  $0x004F, %ax
	jnz   vesa_no_vbe

	/* ---- Fill 0x600 boot FB info from saved block at 0x2400 ---- */
	movw  $0x600, %bx
	movb  $1, (%bx)
	movw  0x2412, %ax
	movw  %ax, 1(%bx)		/* width */
	movw  0x2414, %ax
	movw  %ax, 3(%bx)		/* height */
	movw  0x2410, %ax
	movw  %ax, 5(%bx)		/* pitch */
	movb  0x2419, %al
	movb  %al, 7(%bx)		/* bpp */
	movw  0x2428, %ax
	movw  %ax, 8(%bx)		/* lfb_base lo */
	movw  0x242A, %ax
	movw  %ax, 10(%bx)		/* lfb_base hi */
	movw  vram_64k, %ax
	movw  %ax, 12(%bx)		/* vram_64k */
	jmp   enter_pm

vesa_no_vbe:
	/* No VESA: stay in text mode, mark fb info invalid. */
	movw  $0x600, %bx
	movb  $0, (%bx)

enter_pm:
	/* Enable A20 gate. */
	movw  $0x2401, %ax
	int   $0x15
	call  a20_wait
	movb  $0xD1, %al
	outb  %al, $0x64
	call  a20_wait
	movb  $0xDF, %al
	outb  %al, $0x60
	call  a20_wait
	inb   $0x92, %al
	testb $0x02, %al
	jnz   1f
	orb   $0x02, %al
	andb  $0xFE, %al
	outb  %al, $0x92
1:
	cli
	lgdt  stage2_gdtr
	movl  %cr0, %eax
	orl   $1, %eax
	movl  %eax, %cr0
	.byte 0x66, 0xea
	.long stage2_pm
	.word 0x08

a20_wait:
	inb   $0x64, %al
	testb $0x02, %al
	jnz   a20_wait
	ret

.code32
stage2_pm:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	movw  %ax, %ss
	movl  $0x20800, %esp
	jmp   kernel_main

.align 4
stage2_gdt:
	.quad 0
	.word 0xffff, 0x0000
	.byte 0x00, 0x9a, 0xcf, 0x00
	.word 0xffff, 0x0000
	.byte 0x00, 0x92, 0xcf, 0x00
stage2_gdt_end:
stage2_gdtr:
	.word stage2_gdt_end - stage2_gdt - 1
	.long stage2_gdt

.align 2
vram_64k:
	.word 0
best_mode:
	.word 0
best_score:
	.long 0
