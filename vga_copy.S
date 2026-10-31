/* VGA framebuffer row copy: rep movsl optimized for P5 USWC write-combining.
 *
 * void vga_fb_copy_row(void *dst, void *src, unsigned int dwords);
 *
 * On Pentium (P5), rep movsl to UC memory (PCI framebuffer) uses the
 * 64-byte USWC write-combining buffer. Writes are batched and flushed
 * as burst transfers to VRAM, avoiding per-dword PCI bus stalls.
 */
.code32
.text

.globl vga_fb_copy_row
vga_fb_copy_row:
	pushl %esi
	pushl %edi
	movl  12(%esp), %edi	/* dst */
	movl  16(%esp), %esi	/* src */
	movl  20(%esp), %ecx	/* dword count */
	cld
	rep movsl
	popl %edi
	popl %esi
	ret

/* void vga_fb_fill_row_sse2(void *dst, unsigned int value, unsigned int dwords);
 *
 * 16 bytes of the (broadcast) dword per iteration, then the remaining dwords.
 * Only called when cpuid reported SSE2; unaligned-safe (movdqu). */
.globl vga_fb_fill_row_sse2
vga_fb_fill_row_sse2:
	pushl %edi
	movl  8(%esp), %edi	/* dst */
	movl  12(%esp), %eax	/* value */
	movl  16(%esp), %ecx	/* dword count */
	movd  %eax, %xmm0
	pshufd $0, %xmm0, %xmm0
	movl  %ecx, %edx
	shrl  $2, %edx
	jz    2f
1:	movdqu %xmm0, (%edi)
	addl  $16, %edi
	decl  %edx
	jnz   1b
2:	andl  $3, %ecx
	jz    4f
3:	movl  %eax, (%edi)
	addl  $4, %edi
	decl  %ecx
	jnz   3b
4:	popl  %edi
	ret

/* void vga_fb_fill_row_nt(void *dst, unsigned int value, unsigned int dwords);
 *
 * Streaming (non-temporal) store version for long rows: avoids the
 * read-for-ownership traffic of a cached fill, which is what makes large
 * fills DRAM-bound on the Atom.  Aligns to 16 bytes with normal stores, then
 * movntdq.  The caller must sfence before the data is read back. */
.globl vga_fb_fill_row_nt
vga_fb_fill_row_nt:
	pushl %edi
	movl  8(%esp), %edi	/* dst */
	movl  12(%esp), %eax	/* value */
	movl  16(%esp), %ecx	/* dword count */
	movd  %eax, %xmm0
	pshufd $0, %xmm0, %xmm0
1:	testl $15, %edi
	jz    2f
	testl %ecx, %ecx
	jz    9f
	movl  %eax, (%edi)
	addl  $4, %edi
	decl  %ecx
	jmp   1b
2:	movl  %ecx, %edx
	shrl  $2, %edx
	jz    4f
3:	movntdq %xmm0, (%edi)
	addl  $16, %edi
	decl  %edx
	jnz   3b
4:	andl  $3, %ecx
	jz    9f
5:	movl  %eax, (%edi)
	addl  $4, %edi
	decl  %ecx
	jnz   5b
9:	popl  %edi
	ret

/* void vga_fb_copy_row_nt(void *dst, const void *src, unsigned int dwords);
 *
 * Same as vga_fb_copy_row, but with streaming (non-temporal) stores: WC
 * destinations combine them into bursts, which rep movsl does not do here.
 * The destination is aligned with normal stores first; the caller sfences. */
.globl vga_fb_copy_row_nt
vga_fb_copy_row_nt:
	pushl %esi
	pushl %edi
	movl  12(%esp), %edi	/* dst */
	movl  16(%esp), %esi	/* src */
	movl  20(%esp), %ecx	/* dword count */
1:	testl $15, %edi
	jz    2f
	testl %ecx, %ecx
	jz    9f
	movl  (%esi), %eax
	movl  %eax, (%edi)
	addl  $4, %esi
	addl  $4, %edi
	decl  %ecx
	jmp   1b
2:	movl  %ecx, %edx
	shrl  $2, %edx
	jz    4f
3:	movdqu (%esi), %xmm0
	movntdq %xmm0, (%edi)
	addl  $16, %esi
	addl  $16, %edi
	decl  %edx
	jnz   3b
4:	andl  $3, %ecx
	jz    9f
5:	movl  (%esi), %eax
	movl  %eax, (%edi)
	addl  $4, %esi
	addl  $4, %edi
	decl  %ecx
	jnz   5b
9:	popl  %edi
	popl  %esi
	ret
