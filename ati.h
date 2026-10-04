/* ATI GPU init: Mach64/Rage CRTC fixups + Radeon (R200/RV280) dispatch. */
#ifndef ATI_H
#define ATI_H

/* Called by gpu_init() with the active adapter's bus/device. */
void ati_init(unsigned int bus, unsigned int dev);

/* 1 if the active ATI adapter is a Radeon (needs no Mach64 CRTC fixup). */
int  ati_is_radeon(void);

unsigned int ati_pci_id(void);
unsigned int ati_off_pitch_before(void);
unsigned int ati_off_pitch_after(void);
unsigned int ati_mmio_method(void);

#endif
