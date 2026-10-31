/* NVIDIA GPU support.
 *
 * The VBE mode set (vesa_stage.S) is all the *display* setup NVIDIA cards
 * need; the legacy VGA CRTC registers at 0x3D4/0x3D5 must not be touched
 * (see nvidia.c).  This file identifies the card and maps its register
 * aperture (BAR0) so the chip can be named and the nv50-family (ION/MCP79)
 * display engine can be driven through it.
 */
#ifndef NVIDIA_H
#define NVIDIA_H

/* Identify the NVIDIA adapter at bus/dev and map its register aperture.
 * device_id = high 16 bits of PCI config 0x00 (e.g. 0x087D for ION). */
void nvidia_init(unsigned int bus, unsigned int dev, unsigned int device_id);

/* 1 when the register aperture was mapped and NV_PMC_BOOT_0 looks valid. */
int nvidia_available(void);

/* Human-readable device name ("C79 [ION]", "GM206 [GeForce GTX 960]", ...). */
const char *nvidia_chip_name(void);

/* NV_PMC_BOOT_0 chipset id (0 when MMIO could not be validated). */
unsigned int nvidia_chipset(void);

/* BAR0 register aperture / BAR1 VRAM window / BAR3 VBE window bases
 * (0 = absent) and their probed sizes. */
unsigned int nvidia_bar0(void);
unsigned int nvidia_bar1(void);
unsigned int nvidia_bar1_size(void);
unsigned int nvidia_bar3(void);
unsigned int nvidia_bar3_size(void);

/* Stolen-VRAM geometry (MCP77/79/89): physical base and size in bytes
 * (0 when not applicable).  VRAM addresses used by the display engine are
 * relative to this base. */
unsigned int nvidia_vram_base(void);
unsigned int nvidia_vram_size(void);

/* The VBE LFB is a window into the stolen VRAM, which is ordinary DRAM and
 * therefore much faster to write than the PCIe aperture.  Locates the
 * framebuffer in the stolen VRAM by writing a marker through the LFB and
 * scanning the (now cacheably mapped) VRAM at page granularity.  Returns the
 * framebuffer's address in the stolen VRAM, or 0 when not applicable. */
unsigned int nvidia_lfb_in_vram(void);

#endif
