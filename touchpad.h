/* Elan HID-over-I2C touchpad (HP Chromebook 11 G5 EE / setzer).
 *
 * The controller is a DesignWare I2C block in the Braswell LPSS; the
 * touchpad answers at slave address 0x15 with the HID-over-I2C register
 * protocol (16-bit little-endian register addresses).  The device reports
 * absolute finger positions in the Elan 0x5D vendor report; the driver turns
 * successive positions into pointer deltas.
 */
#ifndef TOUCHPAD_H
#define TOUCHPAD_H

/* Probe the LPSS I2C controllers and bring the touchpad up.  Returns 1 when
 * a device was found.  Must run at boot, before processes clone the kernel
 * page directory (it maps the I2C MMIO window). */
int touchpad_init(void);

int touchpad_present(void);

/* Identification of the probed device (valid when present). */
unsigned int touchpad_addr(void);
unsigned short touchpad_vendor(void);
unsigned short touchpad_product(void);

/* `touchpad` kernel command: print the cached probe result. */
void program_touchpad_main(void);

/* Poll one input report.  Returns 1 when the deltas or buttons changed;
 * values use the mouse layer's PS/2 convention (+dy is up). */
int touchpad_poll(int *buttons, int *dx, int *dy);

#endif
