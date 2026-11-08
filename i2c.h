/* Polled Synopsys DesignWare APB I2C master.  Used by the Braswell LPSS
 * controllers, in particular the one the HID-over-I2C touchpad sits on. */
#ifndef I2C_H
#define I2C_H

/* Probe and initialize the controller at a physical MMIO base.  Returns 0
 * when the DesignWare component type matches. */
int i2c_dw_init(unsigned int base);

/* True when the controller at base looks like a DesignWare I2C block. */
int i2c_dw_present(unsigned int base);

/* TX_ABRT_SOURCE of the last failed transfer (0 = timeout, not a NAK). */
extern unsigned int i2c_last_abort;

/* Raw component-type / reset-release access for the LPSS scan. */
unsigned int i2c_dw_comp_type(unsigned int base);
void i2c_dw_reset_release(unsigned int base);

/* Combined write + repeated-start read, the form HID-over-I2C uses for
 * register-addressed transfers.  Either buffer may be empty. */
int i2c_dw_transfer(unsigned int base, unsigned int addr,
		    const unsigned char *wr, unsigned int wrlen,
		    unsigned char *rd, unsigned int rdlen);

#endif
