/* Physical address ceiling shared by the 32-bit kernel and the UEFI loader. */
#ifndef PHYS_H
#define PHYS_H

/* 512 MB: enough for MuJS plus the browser, while the 4 KB frame bitmap
 * stays 16 KB and the 4 MB identity map needs only 128 PDEs. */
#define MM_MAX_PHYS      (512u * 1024 * 1024)

#endif
