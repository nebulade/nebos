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
