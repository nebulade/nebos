/* Paging: identity-map RAM + the VESA LFB and enable CR0.PG (4 MB pages). */
#ifndef PAGING_H
#define PAGING_H

void paging_init(void);

#endif
