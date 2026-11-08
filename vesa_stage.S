/*
 * VESA real-mode stage: runs at 0x7E00 before protected mode.
 *
 * Called by the boot sector in real mode (CS=0, IP=0x7E00).
 * Probes the E820 map, loads the compressed command-pack rootfs in 16 KB
 * chunks through the 0x3000 staging buffer and copies each chunk straight
 * above the 8 MB VGA-shadow reserve (PACK_HIGH_ADDR) with the BIOS block
 * move (INT 15h AH=87h), walks the VBE mode list and picks the best mode (highest resolution,
 * >= 16 bpp, LFB, fits in VRAM), writes the mode info to 0x600 and the boot
 * info to 0x500, then enables A20, enters protected mode and jumps to
 * kernel_entry.  Because the pack never occupies the low gap behind the
 * kernel, the pack size is bounded by RAM, not by 0x60000..0x9FBFF, and the
 * kernel BSS/boot stack may extend far past 0x60000.
 *
 * If no VESA mode works, boots in text mode (valid=0 at 0x600).
 */

.code16
.section .text.stage2, "ax", @progbits
.globl _vesa_stage

/* E820 map constants (must match mm.h). */
.equ E820_MAGIC,    0xE820E820
.equ E820_BUF_END,  0x1008 + 32*24   /* 32 entries * 24 bytes */

/* Rootfs/boot-info constants (must match rootfs.h / bootinfo.h). */
.equ ROOTFS_VERSION,   1
.equ PACK_MAX,         0x80000       /* 512 KB compressed rootfs cap */
.equ PACK_HIGH_ADDR,   0x900000      /* above the 8 MB VGA-shadow reserve */
.equ BOOT_INFO_MAGIC,  0x4E425349
.equ BOOT_INFO_VERSION, 2
.equ BOOT_INFO_WORDS,  12

_vesa_stage:
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es
	cld
	movb  %dl, stage_drive

	/* Ask the BIOS whether this drive supports EDD LBA reads.  USB sticks
	 * are sometimes presented as floppies (DL=0); the CHS geometry used
	 * below cannot reach a rootfs image there. */
	movb  stage_drive, %dl
	movb  $0x41, %ah
	movw  $0x55AA, %bx
	int   $0x13
	jc    1f
	cmpw  $0xAA55, %bx
	jne   1f
	testb $0x01, %cl
	jz    1f
	movb  $1, use_lba
1:	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es

	/* No UEFI boot-info block on the BIOS path (see bootinfo.h).  The boot
	 * sector used 0x500 as its disk-address packet, so clear the magic;
	 * write_bootinfo fills in the real block before the PM jump. */
	movl  $0, 0x500

	/* ---- Probe INT 15h E820 memory map into 0:0x1000 ----
	 * Layout (see mm.h): [magic u32][count u32][24-byte entries...].
	 * Stored below the kernel (0x7E00) so it survives kexec reloads,
	 * which re-run kernel_main but not this real-mode stage. */
	movl  $E820_MAGIC, 0x1000     /* magic */
	movl  $0, 0x1004              /* count = 0 */
	xorl  %ebx, %ebx              /* continuation value */
	movl  $0x1008, %edi           /* ES:DI -> first entry */
e820_loop:
	xorw  %ax, %ax
	movw  %ax, %es                /* (re)set ES for the BIOS buffer */
	movl  $0xE820, %eax
	movl  $0x534D4150, %edx       /* "SMAP" */
	movl  $24, %ecx
	int   $0x15
	jc    e820_done               /* CF set -> unsupported / done */
	cmpl  $0x534D4150, %eax
	jne   e820_done               /* not "SMAP" -> done */
	cmpb  $20, %cl
	jb    e820_done               /* too short -> stop */
	movl  $0, 20(%edi)            /* zero ACPI attrs (BIOS may write 20 B) */
	incl  0x1004                  /* count++ */
	addl  $24, %edi
	testl %ebx, %ebx
	jz    e820_done               /* continuation == 0 -> done */
	cmpl  $E820_BUF_END, %edi
	jae   e820_done               /* buffer full -> stop */
	jmp   e820_loop
e820_done:

	/* ---- Rootfs: load the command pack directly to high memory ----
	 * The pack is a gzip blob behind a one-sector NBROOT1 header (rootfs.h).
	 * Each 16 KB chunk is read into 0x3000 and copied to PACK_HIGH_ADDR with
	 * INT 15h AH=87h, so the size is no longer limited by the
	 * 0x60000..0x9FBFF low gap.  A20 must be on before the first high write;
	 * the old path enabled it only at stage_finish.  The kernel sanitizes
	 * CR4/EFER after this BIOS call (the X201 leaves translation state
	 * behind). */
	call  enable_a20
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es

	movl  0x7C42, %eax            /* boot-sector pack_lba field */
	movl  %eax, pack_lba
	movl  $0, pack_addr
	movl  $0, pack_bytes

	movw  $msg_pack, %si
	call  print_str
	movzbl stage_drive, %eax
	call  print_hex32
	movw  $msg_lba, %si
	call  print_str
	movl  pack_lba, %eax
	call  print_hex32
	movw  $msg_e, %si
	call  print_str
	movzbl use_lba, %eax
	addl  $'0', %eax
	movb  $0x0E, %ah
	movb  $0x00, %bh
	int   $0x10
	movw  $msg_m, %si
	call  print_str
	cmpb  $0x80, stage_drive
	jb    1f
	movb  $'L', %al
	jmp   2f
1:	movb  $'C', %al
2:	movb  $0x0E, %ah
	movb  $0x00, %bh
	int   $0x10
	movw  $msg_end, %si
	call  print_str

	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es

	/* Read and validate the header, then copy the payload in 16 KB
	 * chunks (32 sectors) through the 0x3000 staging buffer. */
	cmpl  $0, pack_lba
	je    pack_fail

	movl  pack_lba, %eax
	movw  $1, %cx
	call  read_sectors
	jc    pack_fail

	/* Validate the header at 0x3000: "NBROOT1\0", version 1. */
	movl  $0x3000, %esi
	movl  0(%esi), %eax
	cmpl  $0x4F52424E, %eax       /* "NBRO" */
	jne   pack_fail
	movl  4(%esi), %eax
	cmpl  $0x0031544F, %eax       /* "OT1\0" */
	jne   pack_fail
	cmpl  $ROOTFS_VERSION, 8(%esi)
	jne   pack_fail
	movl  12(%esi), %eax          /* pack_bytes */
	testl %eax, %eax
	jz    pack_fail
	cmpl  $PACK_MAX, %eax
	ja    pack_fail
	movl  %eax, pack_bytes
	movw  $msg_dot, %si
	call  print_str

	movl  $0, pack_off
load_loop:
	movl  pack_off, %eax
	cmpl  pack_bytes, %eax
	jae   load_ok
	movl  pack_bytes, %ecx
	subl  %eax, %ecx
	addl  $511, %ecx
	shrl  $9, %ecx
	cmpl  $32, %ecx
	jbe   1f
	movl  $32, %ecx
1:	movl  %ecx, chunk_sectors
	movl  pack_off, %eax
	shrl  $9, %eax
	addl  pack_lba, %eax
	incl  %eax                    /* pack data starts after the header */
	call  read_sectors
	jc    pack_fail
	movl  chunk_sectors, %eax
	shll  $9, %eax
	movl  %eax, copy_len
	movl  $PACK_HIGH_ADDR, %eax
	addl  pack_off, %eax
	movl  %eax, copy_dst
	call  bios_copy
	jc    pack_fail
	movl  chunk_sectors, %eax
	shll  $9, %eax
	addl  %eax, pack_off
	movw  $msg_dot, %si
	call  print_str
	jmp   load_loop

load_ok:
	movl  $PACK_HIGH_ADDR, pack_addr
	movw  $msg_ok, %si
	call  print_str
	jmp   pack_done

pack_fail:
	movw  $msg_fail, %si
	call  print_str
pack_clear:
	movl  $0, pack_addr
	movl  $0, pack_bytes
pack_done:

	/* read_sectors leaves ES at the staging segment; the VBE calls below
	 * expect a normal zero segment. */
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

	/* TODO: Remove this width cap when display scaling is improved. */
	cmpw  $1280, 0x2212
	ja    walk_modes

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
	jmp   stage_finish

vesa_no_vbe:
	/* No VESA: stay in text mode, mark fb info invalid. */
	movw  $0x600, %bx
	movb  $0, (%bx)

stage_finish:
	/* The pack is loaded (A20 is already on); finish boot info and enter
	 * protected mode. */
	call  write_bootinfo
	jmp   enter_pm

/* ---- A20 ---------------------------------------------------------------- */
enable_a20:
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
1:	ret

a20_wait:
	inb   $0x64, %al
	testb $0x02, %al
	jnz   a20_wait
	ret

/* ---- Read sectors via INT 13h ------------------------------------------
 * in:  eax = LBA, cx = sector count
 * out: data at 0x3000, CF set on failure
 * Clobbers all general registers. */
read_sectors:
	movl  %eax, rd_lba
	movw  %cx, rd_left
	movw  $0, rd_off
	movw  $0, rd_tries
read_retry:
	movw  rd_off, %bx
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  $0x0300, %ax
	movw  %ax, %es
	movb  stage_drive, %dl
	cmpb  $0x80, %dl
	jb    read_chs

read_lba:
	/* EDD LBA read */
	movw  $0x10, 0x500
	movw  $1, 0x502
	movw  %bx, 0x504
	movw  $0x0300, 0x506
	movl  rd_lba, %eax
	movl  %eax, 0x508
	movl  $0, 0x50C
	movw  $0x500, %si
	movb  $0x42, %ah
	jmp   read_int

read_chs:
	movw  rd_lba, %ax
	xorw  %dx, %dx
	movw  $18, %di
	divw  %di
	movb  %dl, %cl
	incb  %cl
	xorw  %dx, %dx
	movw  $2, %di
	divw  %di
	movb  %dl, %dh
	movb  %al, %ch
	movb  stage_drive, %dl
	movb  $0x02, %ah
	movb  $1, %al
	movw  rd_off, %bx

read_int:
	int   $0x13
	jnc   read_ok
	/* Reset and retry the same sector a few times. */
	pushw %cx
	movb  stage_drive, %dl
	xorw  %ax, %ax
	int   $0x13
	popw  %cx
	incw  rd_tries
	cmpw  $3, rd_tries
	jb    read_retry
	stc
	ret

read_ok:
	incl  rd_lba
	addw  $0x200, rd_off
	decw  rd_left
	jnz   read_retry
	clc
	ret

/* ---- Print a NUL-terminated string with the BIOS teletype ------------- */
print_str:
	pushw %ax
	pushw %bx
	movb  $0x0E, %ah
	xorw  %bx, %bx
1:	lodsb
	testb %al, %al
	jz    2f
	int   $0x10
	jmp   1b
2:	popw  %bx
	popw  %ax
	ret

/* ---- Copy a chunk to high memory with INT 15h AH=87h ------------------
 * in: copy_dst (24-bit), copy_len; source is 0x3000.
 * Copies in 16-bit words; returns with CF set on failure.  The kernel's
 * paging_prepare_cpu() sanitizes the CR4/EFER state this BIOS call can
 * leave behind on the X201. */
bios_copy:
	movl  copy_dst, %eax
	movw  %ax, bios_gdt + 0x1A      /* dest base 15:0 */
	shrl  $16, %eax
	movb  %al, bios_gdt + 0x1C      /* dest base 23:16 */
	movl  copy_len, %ecx
	addl  $1, %ecx                  /* round up for an odd final byte */
	shrl  $1, %ecx                  /* AH=87h counts words */
	jz    bios_copy_done
	xorw  %ax, %ax
	movw  %ax, %es
	movw  $bios_gdt, %si
	movw  $0x8700, %ax
	int   $0x15
	jc    bios_copy_fail
bios_copy_done:
	clc
	ret
bios_copy_fail:
	stc
	ret

/* ---- Hex print (EAX, 8 digits) ----------------------------------------- */
print_hex32:
	movl  $8, %ecx
	jmp   print_hex

print_hex:
	pushal
	movl  %eax, %ebx
1:	roll  $4, %ebx
	movl  %ebx, %eax
	andl  $0x0F, %eax
	cmpl  $10, %eax
	jb    2f
	addl  $55, %eax
	jmp   3f
2:	addl  $48, %eax
3:	pushl %ebx
	pushl %ecx
	movb  $0x0E, %ah
	xorw  %bx, %bx
	int   $0x10
	popl  %ecx
	popl  %ebx
	loop  1b
	popal
	ret

/* ---- Boot-info block at 0x500 (bootinfo.h) ----------------------------
 * twelve dwords: magic, version, checksum, rsdp, flags, pack_addr,
 * pack_size, reserved[5]; the checksum makes the sum zero. */
write_bootinfo:
	xorw  %ax, %ax
	movw  %ax, %ds
	movl  $BOOT_INFO_MAGIC, 0x500
	movl  $BOOT_INFO_VERSION, 0x504
	movl  $0, 0x508
	movl  $0, 0x50C
	movl  $0, 0x510
	movl  pack_addr, %eax
	movl  %eax, 0x514
	movl  pack_bytes, %eax
	movl  %eax, 0x518
	movl  $0, 0x51C
	movl  $0, 0x520
	movl  $0, 0x524
	movl  $0, 0x528
	movl  $0, 0x52C
	xorl  %eax, %eax
	movl  $0x500, %esi
	movl  $BOOT_INFO_WORDS, %ecx
1:	addl  (%esi), %eax
	addl  $4, %esi
	loop  1b
	negl  %eax
	movl  %eax, 0x508
	ret

/* ---- Protected-mode entry ---------------------------------------------- */
enter_pm:
	cli
	lgdt  stage2_gdtr
	movl  %cr0, %eax
	orl   $1, %eax
	movl  %eax, %cr0
	.byte 0x66, 0xea
	.long stage2_pm
	.word 0x08

.code32
stage2_pm:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	movw  %ax, %ss
	movl  $__boot_stack_top, %esp
	jmp   kernel_entry

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

/* ---- INT 15h AH=87h descriptor table (48 bytes) -----------------------
 * 16 zero bytes, source descriptor at 0x10 (0x3000, 64 KB), destination
 * at 0x18 (patched per chunk), 16 zero bytes. */
.align 16
bios_gdt:
	.quad 0
	.quad 0
	/* source: limit 0xFFFF, base 0x3000, access 0x93, 16-bit data */
	.word 0xFFFF
	.word 0x3000
	.byte 0x00
	.byte 0x93
	.word 0x0000
	/* destination: limit 0xFFFF, base patched, access 0x93 */
	.word 0xFFFF
	.word 0x0000
	.byte 0x00
	.byte 0x93
	.word 0x0000
	.quad 0
	.quad 0

/* ---- Stage variables --------------------------------------------------- */
.align 4
stage_drive:    .byte 0
use_lba:        .byte 0
pack_lba:       .long 0
pack_bytes:     .long 0
pack_addr:      .long 0
pack_off:       .long 0
chunk_sectors:  .long 0
copy_dst:       .long 0
copy_len:       .long 0
rd_lba:         .long 0
rd_off:         .word 0
rd_left:        .word 0
rd_tries:       .word 0

msg_pack:       .asciz "Loading rootfs [d="
msg_lba:        .asciz " lba="
msg_e:          .asciz " e="
msg_m:          .asciz " m="
msg_end:        .asciz "]... "
msg_dot:        .asciz "."
msg_ok:         .asciz " ok\r\n"
msg_fail:       .asciz " failed\r\n"

.align 2
vram_64k:
	.word 0
best_mode:
	.word 0
best_score:
	.long 0
