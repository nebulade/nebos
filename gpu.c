/* GPU vendor detection and vendor-specific init dispatch. */

#include "gpu.h"
#include "pci.h"
#include "ati.h"
#include "cirrus.h"
#include "intel.h"
#include "nvidia.h"

static int gpu_vendor_id;
static unsigned int gpu_pci_id_val;

/*
 * Resolve the *active* display adapter: the device whose framebuffer BAR
 * contains the VBE linear framebuffer base.  A machine can carry several
 * VGA-class devices (e.g. an onboard Cirrus plus a plug-in ATI card), and the
 * one that served the VBE mode set is not necessarily the first one the PCI
 * scan hits.  When no BAR contains the LFB (no VBE mode, or a card that maps
 * its framebuffer through a non-BAR0 window), fall back to the first VGA
 * device found.
 */
static unsigned int gpu_bar_size(unsigned int bus, unsigned int dev, unsigned int bar_reg)
{
	unsigned int bar = pci_cfg_read32(bus, dev, 0, bar_reg);
	unsigned int raw;

	if ((bar & 1) || !(bar & 0xFFFFFFF0))
		return 0;
	pci_cfg_write32(bus, dev, 0, bar_reg, 0xFFFFFFFF);
	raw = pci_cfg_read32(bus, dev, 0, bar_reg);
	pci_cfg_write32(bus, dev, 0, bar_reg, bar);
	return (~(raw & 0xFFFFFFF0)) + 1;
}

void gpu_init(unsigned int lfb_base)
{
	unsigned int bus, dev;
	unsigned int active_bus = 0, active_dev = 0, active_vendor = 0, active_id = 0;
	unsigned int fb_bus = 0, fb_dev = 0, fb_vendor = 0, fb_id = 0;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int class_rev, vendor, bar0, bar0_base, bar0_size;

			if (id == 0xFFFFFFFF || id == 0)
				continue;
			class_rev = pci_cfg_read32(bus, dev, 0, 0x08);
			if ((class_rev >> 24) != 0x03)
				continue;

			vendor = id & 0xFFFF;

			/* Remember the first VGA device as a fallback. */
			if (!active_vendor) {
				active_bus = bus;
				active_dev = dev;
				active_vendor = vendor;
				active_id = id;
			}

			/* Match the framebuffer BAR against the LFB base. */
			bar0 = pci_cfg_read32(bus, dev, 0, 0x10);
			if (lfb_base && !(bar0 & 1) && (bar0 & 0xFFFFFFF0)) {
				bar0_base = bar0 & 0xFFFFFFF0;
				bar0_size = gpu_bar_size(bus, dev, 0x10);
				if (bar0_size && lfb_base >= bar0_base &&
				    lfb_base < bar0_base + bar0_size) {
					fb_bus = bus;
					fb_dev = dev;
					fb_vendor = vendor;
					fb_id = id;
				}
			}
		}
	}

	if (fb_vendor) {
		active_bus = fb_bus;
		active_dev = fb_dev;
		active_vendor = fb_vendor;
		active_id = fb_id;
	}

	if (!active_vendor) {
		gpu_vendor_id = GPU_VENDOR_UNKNOWN;
		gpu_pci_id_val = 0;
		return;
	}

	gpu_pci_id_val = active_id;

	if (active_vendor == PCI_VENDOR_ATI) {
		gpu_vendor_id = GPU_VENDOR_ATI;
		ati_init(active_bus, active_dev);
	} else if (active_vendor == PCI_VENDOR_NVIDIA) {
		gpu_vendor_id = GPU_VENDOR_NVIDIA;
		nvidia_init(active_bus, active_dev,
			    (active_id >> 16) & 0xFFFF);
	} else if (active_vendor == PCI_VENDOR_INTEL) {
		gpu_vendor_id = GPU_VENDOR_INTEL;
		intel_init(active_bus, active_dev, (active_id >> 16) & 0xFFFF);
	} else if (active_vendor == PCI_VENDOR_CIRRUS) {
		gpu_vendor_id = GPU_VENDOR_CIRRUS;
		cirrus_init();
	} else {
		gpu_vendor_id = GPU_VENDOR_OTHER;
	}
}

int gpu_vendor(void)
{
	return gpu_vendor_id;
}

unsigned int gpu_pci_id(void)
{
	return gpu_pci_id_val;
}

const char *gpu_vendor_name(void)
{
	switch (gpu_vendor_id) {
	case GPU_VENDOR_ATI:     return "ATI/AMD";
	case GPU_VENDOR_NVIDIA:  return "NVIDIA";
	case GPU_VENDOR_INTEL:   return "Intel";
	case GPU_VENDOR_CIRRUS:  return "Cirrus Logic";
	case GPU_VENDOR_OTHER:   return "Other";
	default:                 return "Unknown";
	}
}
