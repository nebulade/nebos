/* ATI Mach64 / Rage: GPU-specific CRTC fixups via PCI MMIO. */
#ifndef ATI_H
#define ATI_H

void ati_init(void);
unsigned int ati_pci_id(void);
unsigned int ati_off_pitch_before(void);
unsigned int ati_off_pitch_after(void);
unsigned int ati_mmio_method(void);

#endif
