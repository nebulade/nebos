/* NebOS UEFI loader (x86-64).
 *
 * The kernel is a 32-bit i586 image linked at 0x7E00.  This loader gives it
 * the same two boot structures the real-mode VESA stage produces:
 *
 *   0x500  struct boot_info      (EFI flag, GOP pixel order, ACPI RSDP)
 *   0x600  struct vga_boot_fb_info
 *   0x1000 struct e820_map
 *
 * then leaves long mode and jumps to kernel_entry with paging off and flat
 * 32-bit segments.  Nothing in the legacy boot chain is used here.
 */

#include "efi.h"
#include "bootinfo.h"
#include "vga.h"
#include "efi_entry.h"

#define MAX_PHYS    0x08000000u          /* MM_MAX_PHYS in mm.h */
#define PAGE_SIZE_  0x1000u
#define MAX_PAGES   (MAX_PHYS / PAGE_SIZE_)
#define E820_ADDR   0x1000u

/* The kernel image is linked to live at 0x7E00.  Its file image must stay
 * below the legacy pack staging gap at 0x60000 (kernel.ld asserts
 * __bss_start <= 0x60000); KERNEL_MAX is a generous sanity bound. */
#define KERNEL_DEST 0x7E00u
#define KERNEL_MAX  0x70000u

/* Trampoline copy address: below the kernel, same low-memory slot kexec uses
 * (trampoline at 0x800, flat GDT inside the blob, stack at 0x7C00). */
#define TRAMP_ADDR  0x800u
#define TRAMP_MAX   0x800u

extern unsigned char kernel_image_start[];
extern unsigned char kernel_image_end[];
extern unsigned char pack_image_start[];
extern unsigned char pack_image_end[];
extern unsigned char tramp_blob_start[];
extern unsigned char tramp_blob_end[];

/* GetMemoryMap buffer.  A firmware map is normally a few KB; 64 KB is very
 * generous and keeps the retry loop simple. */
static UINT8 memmap_buf[65536] __attribute__((aligned(8)));

/* One bit per 4 KB page over the kernel's 128 MB identity window.  Starts
 * all-used; only EfiConventional/BootServices memory is freed.  EfiLoader*
 * (this image and the staging buffer) stays reserved, which is safe and
 * costs only a few hundred KB. */
static UINT8 page_used[MAX_PAGES / 8];

struct fb_saved {
	int      valid;
	int      rgbx;
	UINT32   width;
	UINT32   height;
	UINT32   pitch;
	UINT32   bpp;
	UINT64   base;
	UINT64   size;
};

struct e820_entry_disk {
	UINT64 base;
	UINT64 length;
	UINT32 type;
	UINT32 attrs;
} __attribute__((packed));

/* ---- serial console (COM1, 115200 8N1) --------------------------------- */

static inline void outb(UINT16 port, UINT8 v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline UINT8 inb(UINT16 port)
{
	UINT8 v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static void com1_init(void)
{
	outb(0x3F9, 0x00);   /* IER */
	outb(0x3FB, 0x80);   /* LCR: DLAB */
	outb(0x3F8, 0x01);   /* DLL: 115200 */
	outb(0x3F9, 0x00);   /* DLM */
	outb(0x3FB, 0x03);   /* LCR: 8N1 */
	outb(0x3FA, 0xC7);   /* FIFO */
	outb(0x3FC, 0x0B);   /* MCR: DTR/RTS/OUT2 */
}

static void com1_putc(char c)
{
	unsigned int guard = 100000;

	while (!(inb(0x3FD) & 0x20) && guard--)
		;
	outb(0x3F8, (UINT8)c);
}

static void com1_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			com1_putc('\r');
		com1_putc(*s++);
	}
}

static void com1_hex(UINT64 v, unsigned int digits)
{
	static const char h[] = "0123456789ABCDEF";
	unsigned int i;

	for (i = digits; i > 0; i--)
		com1_putc(h[(v >> (4 * (i - 1))) & 0xF]);
}

static void com1_dec(UINT32 v)
{
	char buf[11];
	unsigned int i = 0;

	if (v == 0) {
		com1_putc('0');
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i)
		com1_putc(buf[--i]);
}

static void conout_puts(EFI_SYSTEM_TABLE *st, const char *s)
{
	CHAR16 buf[128];
	unsigned int i = 0;

	if (!st || !st->ConOut || !st->ConOut->OutputString)
		return;
	while (*s) {
		if (*s == '\n')
			buf[i++] = '\r';
		buf[i++] = (CHAR16)(UINT8)*s++;
		if (i >= 126) {
			buf[i] = 0;
			st->ConOut->OutputString(st->ConOut, buf);
			i = 0;
		}
	}
	if (i) {
		buf[i] = 0;
		st->ConOut->OutputString(st->ConOut, buf);
	}
}

static void halt_forever(void) __attribute__((noreturn));
static void halt_forever(void)
{
	for (;;)
		__asm__ volatile ("cli; hlt");
}

static void fail_before(EFI_SYSTEM_TABLE *st, const char *msg)
	__attribute__((noreturn));
static void fail_before(EFI_SYSTEM_TABLE *st, const char *msg)
{
	com1_puts("nebos-efi: ");
	com1_puts(msg);
	com1_puts("\n");
	conout_puts(st, "NebOS EFI loader: ");
	conout_puts(st, msg);
	conout_puts(st, "\n");
	halt_forever();
}

static void fail_after(const char *msg) __attribute__((noreturn));
static void fail_after(const char *msg)
{
	com1_puts("nebos-efi: ");
	com1_puts(msg);
	com1_puts("\n");
	halt_forever();
}

/* ---- helpers ----------------------------------------------------------- */

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
	const UINT8 *pa = (const UINT8 *)a;
	const UINT8 *pb = (const UINT8 *)b;
	unsigned int i;

	for (i = 0; i < 16; i++)
		if (pa[i] != pb[i])
			return 0;
	return 1;
}

static UINT32 find_rsdp(EFI_SYSTEM_TABLE *st)
{
	UINTN i;

	if (!st->ConfigurationTable)
		return 0;
	for (i = 0; i < st->NumberOfTableEntries; i++) {
		EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
		UINT64 addr;

		if (!guid_eq(&ct->VendorGuid, &efi_acpi20_guid) &&
		    !guid_eq(&ct->VendorGuid, &efi_acpi10_guid))
			continue;
		addr = (UINT64)(UINTN)ct->VendorTable;
		if (addr && addr <= 0xFFFFFFFFull)
			return (UINT32)addr;
	}
	return 0;
}

/* ---- GOP --------------------------------------------------------------- */

static int gop_init(EFI_SYSTEM_TABLE *st, EFI_GRAPHICS_OUTPUT_PROTOCOL **out_gop,
		    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *out_info)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
	EFI_STATUS status;
	UINTN size;
	UINT32 i, best = 0, best_score = 0;
	UINT64 fb_base, fb_size;

	status = st->BootServices->LocateProtocol((EFI_GUID *)&efi_gop_guid,
						  0, (void **)&gop);
	if (status || !gop || !gop->Mode)
		return 0;

	for (i = 0; i < gop->Mode->MaxMode; i++) {
		UINT64 bytes;
		UINT32 score;

		status = gop->QueryMode(gop, i, &size, &info);
		if (status || !info)
			continue;
		if (info->PixelFormat != EFI_PIXEL_BGRX &&
		    info->PixelFormat != EFI_PIXEL_RGBX)
			continue;
		if (info->HorizontalResolution < 320 ||
		    info->VerticalResolution < 200)
			continue;
		if (info->HorizontalResolution > 1280 ||
		    info->VerticalResolution > 65535)
			continue;
		if (info->PixelsPerScanLine < info->HorizontalResolution)
			continue;
		if (info->PixelsPerScanLine > 0x3FFF)  /* pitch must fit u16 */
			continue;
		bytes = (UINT64)info->PixelsPerScanLine * 4 *
			info->VerticalResolution;
		if (bytes > 8ull * 1024 * 1024)
			continue;
		score = info->HorizontalResolution * info->VerticalResolution;
		if (info->PixelFormat == EFI_PIXEL_BGRX)
			score++;   /* prefer BGRX on equal area */
		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}
	if (!best_score)
		return 0;

	status = gop->SetMode(gop, best);
	if (status)
		return 0;

	info = gop->Mode->Info;
	if (!info)
		return 0;
	if (info->PixelFormat != EFI_PIXEL_BGRX &&
	    info->PixelFormat != EFI_PIXEL_RGBX)
		return 0;
	if (info->HorizontalResolution < 320 ||
	    info->HorizontalResolution > 1280 ||
	    info->VerticalResolution < 200 ||
	    info->VerticalResolution > 65535 ||
	    info->PixelsPerScanLine < info->HorizontalResolution ||
	    info->PixelsPerScanLine > 0x3FFF)
		return 0;
	{
		UINT64 bytes = (UINT64)info->PixelsPerScanLine * 4 *
			info->VerticalResolution;

		if (bytes > 8ull * 1024 * 1024)
			return 0;
	}
	fb_base = gop->Mode->FrameBufferBase;
	fb_size = gop->Mode->FrameBufferSize;
	if (!fb_base || fb_base > 0xFFFFFFFFull)
		return 0;
	if (fb_base + fb_size > 0x100000000ull)
		return 0;

	out_info->Version = info->Version;
	out_info->HorizontalResolution = info->HorizontalResolution;
	out_info->VerticalResolution = info->VerticalResolution;
	out_info->PixelFormat = info->PixelFormat;
	out_info->PixelInformation.RedMask = info->PixelInformation.RedMask;
	out_info->PixelInformation.GreenMask = info->PixelInformation.GreenMask;
	out_info->PixelInformation.BlueMask = info->PixelInformation.BlueMask;
	out_info->PixelInformation.ReservedMask =
		info->PixelInformation.ReservedMask;
	out_info->PixelsPerScanLine = info->PixelsPerScanLine;
	*out_gop = gop;
	return 1;
}

/* ---- E820 conversion --------------------------------------------------- */

static inline int page_is_used(UINT32 p)
{
	return (page_used[p >> 3] >> (p & 7)) & 1;
}

static inline void page_set_used(UINT32 p)
{
	page_used[p >> 3] |= (UINT8)(1u << (p & 7));
}

static inline void page_set_free(UINT32 p)
{
	page_used[p >> 3] &= (UINT8)~(1u << (p & 7));
}

/* Free only pages fully inside [base, base+len). */
static void free_range(UINT64 base, UINT64 len)
{
	UINT64 end;
	UINT32 p0, p1, p;

	if (base >= MAX_PHYS)
		return;
	end = base + len;
	if (end > MAX_PHYS)
		end = MAX_PHYS;
	p0 = (UINT32)((base + PAGE_SIZE_ - 1) / PAGE_SIZE_);
	p1 = (UINT32)(end / PAGE_SIZE_);
	for (p = p0; p < p1; p++)
		page_set_free(p);
}

/* Reserve every page touched by [base, base+len). */
static void use_range(UINT64 base, UINT64 len)
{
	UINT64 end;
	UINT32 p0, p1, p;

	if (base >= MAX_PHYS)
		return;
	end = base + len;
	if (end > MAX_PHYS)
		end = MAX_PHYS;
	p0 = (UINT32)(base / PAGE_SIZE_);
	p1 = (UINT32)((end + PAGE_SIZE_ - 1) / PAGE_SIZE_);
	for (p = p0; p < p1; p++)
		page_set_used(p);
}

static void build_e820(EFI_MEMORY_DESCRIPTOR *map, UINTN map_size,
		       UINTN desc_size, const struct fb_saved *fb)
{
	volatile UINT32 *hdr = (volatile UINT32 *)E820_ADDR;
	volatile struct e820_entry_disk *entries =
		(volatile struct e820_entry_disk *)(E820_ADDR + 8);
	UINTN n = desc_size ? map_size / desc_size : 0;

	__asm__ volatile ("" : "+r"(hdr), "+r"(entries));
	UINTN i;
	UINT32 p, count = 0;
	UINT64 total = 0;

	for (i = 0; i < sizeof(page_used); i++)
		page_used[i] = 0xFF;

	for (i = 0; i < n; i++) {
		EFI_MEMORY_DESCRIPTOR *d =
			(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
		UINT64 len = d->NumberOfPages * PAGE_SIZE_;

		if (d->Type == EFI_CONVENTIONAL_MEMORY ||
		    d->Type == EFI_BOOT_SERVICES_CODE ||
		    d->Type == EFI_BOOT_SERVICES_DATA)
			free_range(d->PhysicalStart, len);
	}

	/* The scanout is MMIO, not RAM; make sure it can never be handed out
	 * even when its BAR happens to sit below the 128 MB ceiling. */
	if (fb->valid)
		use_range(fb->base, fb->size);

	for (p = 0; p < MAX_PAGES; ) {
		UINT32 start;

		if (page_is_used(p)) {
			p++;
			continue;
		}
		start = p;
		while (p < MAX_PAGES && !page_is_used(p))
			p++;
		if (count < 32) {
			UINT64 len = (UINT64)(p - start) * PAGE_SIZE_;

			entries[count].base = (UINT64)start * PAGE_SIZE_;
			entries[count].length = len;
			entries[count].type = 1;   /* E820_RAM */
			entries[count].attrs = 0;
			count++;
			total += len;
		}
	}

	hdr[0] = 0xE820E820u;
	hdr[1] = count;

	com1_puts("nebos-efi: E820 ");
	com1_dec(count);
	com1_puts(" entries, ");
	com1_dec((UINT32)(total >> 20));
	com1_puts(" MB usable\n");
}

/* ---- boot structures --------------------------------------------------- */

/* Copy the embedded rootfs pack to the top of RAM (below the 128 MB
 * identity window and above the 1..9 MB VGA shadow reserve), matching the
 * legacy stage's placement.  Returns the address, or 0 when it does not fit.
 * Runs after ExitBootServices, so only plain memory accesses. */
static UINT32 copy_pack(UINT32 size)
{
	volatile UINT32 *map = (volatile UINT32 *)E820_ADDR;
	volatile struct e820_entry_disk *entries =
		(volatile struct e820_entry_disk *)(E820_ADDR + 8);
	UINT32 need, count, i, best = 0;
	UINT32 p;

	if (size == 0 || size > 0x400000u)
		return 0;
	need = (size + PAGE_SIZE_ - 1) & ~(PAGE_SIZE_ - 1);
	if (map[0] != 0xE820E820u)
		return 0;
	count = map[1];
	if (count > 32)
		count = 32;

	for (i = 0; i < count; i++) {
		UINT64 base = entries[i].base;
		UINT64 end = base + entries[i].length;
		UINT32 a;

		if (entries[i].type != 1 || base >= MAX_PHYS)
			continue;
		if (end > MAX_PHYS)
			end = MAX_PHYS;
		if (end < base + need)
			continue;
		a = (UINT32)(end - need) & ~(PAGE_SIZE_ - 1);
		if (a < 0x900000u || a <= best)
			continue;
		best = a;
	}
	if (!best)
		return 0;

	for (p = 0; p < size; p++)
		((UINT8 *)(UINTN)best)[p] = pack_image_start[p];
	return best;
}

static void write_bootinfo(UINT32 rsdp, int have_fb, int rgbx,
			   UINT32 pack_addr, UINT32 pack_size)
{
	volatile struct boot_info *bi =
		(volatile struct boot_info *)BOOT_INFO_ADDR;
	UINT32 words[BOOT_INFO_WORDS];
	unsigned int i;
	UINT32 sum = 0;

	__asm__ volatile ("" : "+r"(bi));

	for (i = 0; i < BOOT_INFO_WORDS; i++)
		words[i] = 0;
	words[0] = BOOT_INFO_MAGIC;
	words[1] = BOOT_INFO_VERSION;
	words[3] = rsdp;
	words[4] = BOOTINFO_FLAG_EFI |
		((have_fb && rgbx) ? BOOTINFO_FLAG_RGBX : 0);
	words[5] = pack_addr;
	words[6] = pack_size;
	for (i = 0; i < BOOT_INFO_WORDS; i++)
		sum += words[i];
	words[2] = 0u - sum;   /* the sum over the block is zero */

	bi->magic = words[0];
	bi->version = words[1];
	bi->checksum = words[2];
	bi->rsdp = words[3];
	bi->flags = words[4];
	bi->pack_addr = words[5];
	bi->pack_size = words[6];
	for (i = 0; i < 5; i++)
		bi->reserved[i] = words[7 + i];
}

static void write_fbinfo(const struct fb_saved *fb)
{
	volatile struct vga_boot_fb_info *info = VGA_BOOT_FB;

	__asm__ volatile ("" : "+r"(info));
	if (!fb->valid) {
		info->valid = 0;
		info->width = 0;
		info->height = 0;
		info->pitch = 0;
		info->bpp = 0;
		info->lfb_base = 0;
		info->vram_64k = 0;
		return;
	}

	info->valid = 1;
	info->width = (unsigned short)fb->width;
	info->height = (unsigned short)fb->height;
	info->pitch = (unsigned short)fb->pitch;
	info->bpp = (unsigned char)fb->bpp;
	info->lfb_base = (unsigned int)fb->base;
	{
		UINT64 blocks = fb->size >> 16;

		info->vram_64k = (unsigned short)(blocks > 0xFFFF ? 0xFFFF :
						  blocks);
	}
}

static void copy_trampoline(void)
{
	UINT32 n = (UINT32)(tramp_blob_end - tramp_blob_start);
	UINT8 *dst = (UINT8 *)(UINTN)TRAMP_ADDR;
	UINT32 i;

	if (n == 0 || n > TRAMP_MAX)
		fail_after("trampoline blob size invalid");
	__asm__ volatile ("" : "+r"(dst));
	for (i = 0; i < n; i++)
		dst[i] = tramp_blob_start[i];
	com1_puts("nebos-efi: trampoline ");
	com1_dec(n);
	com1_puts(" bytes at 0x");
	com1_hex(TRAMP_ADDR, 3);
	com1_puts("\n");
}

/* ---- entry ------------------------------------------------------------- */

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	EFI_BOOT_SERVICES *bs;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
	struct fb_saved fb;
	EFI_PHYSICAL_ADDRESS staging = 0xFFFFFFFFull;
	UINTN map_size, map_key = 0, desc_size = 0;
	UINT32 desc_ver = 0;
	UINT32 rsdp;
	UINT32 kernel_size;
	UINT32 pack_addr = 0, pack_size;
	UINTN pages, i, tries;
	EFI_STATUS status;

	com1_init();
	com1_puts("nebos-efi: start\n");

	if (!st || !st->BootServices)
		fail_before(st, "no boot services");
	bs = st->BootServices;

	bs->SetWatchdogTimer(0, 0, 0, 0);

	kernel_size = (UINT32)(kernel_image_end - kernel_image_start);
	if (kernel_size == 0 || kernel_size > KERNEL_MAX)
		fail_before(st, "kernel image size invalid");
	pack_size = (UINT32)(pack_image_end - pack_image_start);
	com1_puts("nebos-efi: kernel ");
	com1_dec(kernel_size);
	com1_puts(" bytes, entry 0x");
	com1_hex((UINT64)KERNEL_ENTRY, 8);
	com1_puts(", rootfs ");
	com1_dec(pack_size);
	com1_puts(" bytes\n");

	/* Video: prefer BGRX (the byte order the kernel's shadow buffer uses);
	 * RGBX is handled by a swizzling flush. */
	fb.valid = 0;
	fb.rgbx = 0;
	if (gop_init(st, &gop, &info)) {
		fb.valid = 1;
		fb.rgbx = (info.PixelFormat == EFI_PIXEL_RGBX);
		fb.width = info.HorizontalResolution;
		fb.height = info.VerticalResolution;
		fb.pitch = info.PixelsPerScanLine * 4;
		fb.bpp = 32;
		fb.base = gop->Mode->FrameBufferBase;
		fb.size = gop->Mode->FrameBufferSize;
		com1_puts("nebos-efi: GOP ");
		com1_dec(fb.width);
		com1_puts("x");
		com1_dec(fb.height);
		com1_puts(fb.rgbx ? " RGBX" : " BGRX");
		com1_puts(" pitch ");
		com1_dec(fb.pitch);
		com1_puts(" fb 0x");
		com1_hex(fb.base, 8);
		com1_puts("\n");
	} else {
		com1_puts("nebos-efi: no usable GOP mode, booting headless\n");
	}

	rsdp = find_rsdp(st);
	if (rsdp) {
		com1_puts("nebos-efi: ACPI RSDP 0x");
		com1_hex(rsdp, 8);
		com1_puts("\n");
	}

	/* Stage the kernel below 4 GB; the low-memory trampoline copies it to
	 * 0x7E00 after ExitBootServices, when low memory is ours. */
	pages = (kernel_size + PAGE_SIZE_ - 1) / PAGE_SIZE_;
	status = bs->AllocatePages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
				   pages, &staging);
	if (status || staging > 0xFFFFFFFFull || staging < 0x100000ull ||
	    staging + kernel_size > 0x100000000ull)
		fail_before(st, "cannot allocate kernel staging buffer");
	for (i = 0; i < kernel_size; i++)
		((UINT8 *)(UINTN)staging)[i] = kernel_image_start[i];
	com1_puts("nebos-efi: staged at 0x");
	com1_hex(staging, 8);
	com1_puts("\n");

	/* GetMemoryMap + ExitBootServices.  A map-key change makes the call
	 * fail; fetch a fresh map and retry. */
	for (tries = 0; tries < 4; tries++) {
		map_size = sizeof(memmap_buf);
		status = bs->GetMemoryMap(&map_size,
					  (EFI_MEMORY_DESCRIPTOR *)memmap_buf,
					  &map_key, &desc_size, &desc_ver);
		if (status) {
			com1_puts("nebos-efi: GetMemoryMap status 0x");
			com1_hex(status, 16);
			com1_puts("\n");
			fail_before(st, "GetMemoryMap failed");
		}
		status = bs->ExitBootServices(image, map_key);
		if (!status)
			break;
	}
	if (status)
		fail_before(st, "ExitBootServices failed");

	/* From here on no firmware services are allowed.  The firmware IDT and
	 * timer are still live until the kernel installs its own, so keep
	 * interrupts off. */
	__asm__ volatile ("cli");
	com1_puts("nebos-efi: boot services exited\n");

	if (!desc_size || map_size < desc_size)
		fail_after("memory map is empty");
	build_e820((EFI_MEMORY_DESCRIPTOR *)memmap_buf, map_size, desc_size,
		   &fb);
	pack_addr = copy_pack(pack_size);
	com1_puts("nebos-efi: rootfs at 0x");
	com1_hex(pack_addr, 8);
	com1_puts("\n");
	write_bootinfo(rsdp, fb.valid, fb.rgbx, pack_addr,
		       pack_addr ? pack_size : 0);
	write_fbinfo(&fb);
	copy_trampoline();

	com1_puts("nebos-efi: jumping to kernel\n");
	((void (*)(UINT32, UINT32, UINT32))(UINTN)TRAMP_ADDR)
		((UINT32)staging, kernel_size, (UINT32)KERNEL_ENTRY);

	/* The trampoline never returns. */
	halt_forever();
	return EFI_SUCCESS;
}
