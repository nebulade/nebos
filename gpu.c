/* GPU vendor detection and vendor-specific init dispatch. */

#include "gpu.h"
#include "pci.h"
#include "ati.h"
#include "cirrus.h"
#include "nvidia.h"

static int gpu_vendor_id;
static unsigned int gpu_pci_id_val;

void gpu_init(void)
{
	unsigned int bus, dev, id, class_rev;
	unsigned int vendor;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			id = pci_cfg_read32(bus, dev, 0, 0x00);
			if (id == 0xFFFFFFFF || id == 0)
				continue;
			class_rev = pci_cfg_read32(bus, dev, 0, 0x08);
			if ((class_rev >> 24) != 0x03)
				continue;

			gpu_pci_id_val = id;
			vendor = id & 0xFFFF;

			if (vendor == PCI_VENDOR_ATI) {
				gpu_vendor_id = GPU_VENDOR_ATI;
				ati_init();
			} else if (vendor == PCI_VENDOR_NVIDIA) {
				gpu_vendor_id = GPU_VENDOR_NVIDIA;
				nvidia_init();
			} else if (vendor == PCI_VENDOR_INTEL) {
				gpu_vendor_id = GPU_VENDOR_INTEL;
			} else if (vendor == PCI_VENDOR_CIRRUS) {
				gpu_vendor_id = GPU_VENDOR_CIRRUS;
				cirrus_init();
			} else {
				gpu_vendor_id = GPU_VENDOR_OTHER;
			}
			return;
		}
	}
	gpu_vendor_id = GPU_VENDOR_UNKNOWN;
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
