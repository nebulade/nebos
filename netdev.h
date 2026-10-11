/* Network device interface: one struct per NIC driver, shared by the network
 * stack (net.c) and the net* shell commands (netcmd.c).
 *
 * Drivers are registered in the table in netdev.c (first match wins); add new
 * ones there.  Everything here is polled, matching the rest of the kernel. */
#ifndef NETDEV_H
#define NETDEV_H

struct netdev {
	const char *name;    /* short driver name, e.g. "ne2000" */
	const char *chip;    /* detected chip, e.g. "RTL8029AS" */
	int  (*init)(void);  /* probe + bring up: 0 = ready, -1 = not present */
	void (*get_mac)(unsigned char mac[6]);
	int  (*rx)(unsigned char *buf, unsigned int maxlen);    /* len, or -1 */
	int  (*tx)(const unsigned char *buf, unsigned int len); /* 0, or -1 */
	void (*print_info)(void);   /* status lines (MAC, link, ...) */
	void (*dump_regs)(void);    /* register dump; may be NULL */
	int  (*set_loopback)(int on);   /* PHY loopback for self-test; may be NULL */
	void (*reset)(void);        /* full NIC re-init (recover from a stall); may be NULL */
};

/* First registered driver that probes successfully (probe result is cached);
 * NULL when no NIC is present. */
const struct netdev *netdev_active(void);

/* Busy gate for commands that poll the NIC for a while (netrecv, ping,
 * netping, netloop, netkexec): the background kexec watcher must not steal
 * frames while one of them owns the receiver. */
void netdev_acquire(void);
void netdev_release(void);
int  netdev_in_use(void);

/* Fill the active NIC's MAC; -1 (and a zero MAC) when there is no NIC. */
int netdev_get_mac(unsigned char mac[6]);

/* One shared frame-receive scratch buffer (1518 bytes) for protocol code that
 * just needs somewhere for nd->rx() to land.  Only valid while the caller
 * owns the NIC, and only until the next rx().  Shared so the DNS/TCP/ARP
 * paths don't each carry their own frame buffer in the kernel's BSS. */
unsigned char *netdev_rx_scratch(void);
#define NETDEV_FRAME_MAX 1518

#endif
