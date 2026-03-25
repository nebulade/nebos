/*
 * NVIDIA GPU init.
 *
 * Unlike ATI Mach64/Rage which need manual CRTC_OFF_PITCH fixups,
 * NVIDIA GeForce cards (especially Kepler and newer like GTX 960/GM206)
 * handle VBE mode sets cleanly through their VBIOS.  The standard VGA
 * CRTC registers at 0x3D4/0x3D5 should NOT be touched -- NVIDIA's
 * display engine ignores or misinterprets legacy VGA CRTC writes,
 * which can cause display corruption or system reboot.
 *
 * For now, this is intentionally a no-op.  The VBE mode set from
 * boot.S provides a working LFB and the card needs no further fixup.
 */

#include "nvidia.h"

void nvidia_init(void)
{
}
