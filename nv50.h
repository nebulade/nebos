/* NVIDIA nv50-family (ION/MCP79) display-engine diagnostics.
 *
 * The hardware cursor is not driven on this family (the VBIOS keeps the
 * display engine; see nv50.c for the details).  This is a boot-time,
 * read-only probe of the engine state for serial / `make netmon`. */
#ifndef NV50_H
#define NV50_H

void nv50_probe(void);

#endif
