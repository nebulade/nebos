/* ACPI embedded-controller access (ports 0x62/0x66).
 *
 * The kernel has no ACPI interpreter, but the standard EC command interface
 * is simple enough to drive directly.  Both the ThinkPad H8 (tpec.c) and the
 * Chrome OS EC (cros_ec.c) sit behind the same protocol:
 *
 *   0x80 read:  command 0x80, register index, read the data byte
 *   0x81 write: command 0x81, register index, value
 *
 * The status port carries IBF (input buffer full) and OBF (output buffer
 * full).  A pending output byte (an EC event) is drained before each command,
 * and every wait is bounded so a dead controller cannot stall the kernel.
 */
#ifndef EC_H
#define EC_H

/* Read one EC RAM byte.  Returns 0 on success, -1 when the EC did not
 * answer (no controller, timeout, or a stale output byte that never
 * cleared). */
int ec_read(unsigned char reg, unsigned char *out);

/* Write one EC RAM byte (ACPI "write EC RAM", command 0x81). */
int ec_write(unsigned char reg, unsigned char val);

/* Read a little-endian 16-bit value from two consecutive EC registers. */
int ec_read16(unsigned char reg, unsigned int *out);

#endif
