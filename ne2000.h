/* NE2000-compatible (Realtek RTL8029AS) Ethernet driver.
   Polling based (no IRQ), byte-mode remote DMA, on-card 16 KB SRAM ring. */
#ifndef NE2000_H
#define NE2000_H

/* Probe + reset + configure the NIC. Idempotent; 0 on success, -1 if absent. */
int ne2000_init(void);

/* 1 if a supported NIC was found. */
int ne2000_present(void);

/* Copy the NIC's 6-byte MAC address into out[]. */
void ne2000_get_mac(unsigned char out[6]);

/* Print NIC info (base, MAC) to the console. */
void ne2000_print_info(void);

/* Poll one received frame. Returns length (>= 14) or -1 if none pending.
   buf must hold at least maxlen bytes; frames longer than maxlen are skipped. */
int ne2000_rx(unsigned char *buf, unsigned int maxlen);

/* Transmit a raw Ethernet frame (must include the 14-byte header).
   Returns 0 on success, -1 on error. */
int ne2000_tx(const unsigned char *buf, unsigned int len);

#endif
