/* GPU vendor detection and unified init. */
#ifndef GPU_H
#define GPU_H

#define GPU_VENDOR_UNKNOWN  0
#define GPU_VENDOR_ATI      1
#define GPU_VENDOR_NVIDIA   2
#define GPU_VENDOR_INTEL    3
#define GPU_VENDOR_OTHER    4

#define PCI_VENDOR_ATI      0x1002
#define PCI_VENDOR_NVIDIA   0x10DE
#define PCI_VENDOR_INTEL    0x8086

void gpu_init(void);
int gpu_vendor(void);
unsigned int gpu_pci_id(void);
const char *gpu_vendor_name(void);

#endif
