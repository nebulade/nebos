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
	int  (*set_speed)(unsigned int mbps); /* 0 = auto, 100 = force 100M; may be NULL */
	void (*set_mac)(const unsigned char mac[6]); /* runtime MAC override; may be NULL */
	/* Address applied when this driver becomes active.  The direct ASIX
	 * link uses its own subnet so it can coexist with the e1000e link to
	 * the router.  NULL leaves the current address alone. */
	const unsigned char *default_ip;
	const unsigned char *default_mask;
	/* Short RX summary for the boot log (a full register dump can
	 * overflow the netlog ring before the WM starts); may be NULL. */
	void (*dump_brief)(void);
};

/* First registered driver that probes successfully (probe result is cached);
 * NULL when no NIC is present. */
const struct netdev *netdev_active(void);

/* Management NIC for netlog/netkexec.  Prefers the ASIX dongle so the
 * direct debug link survives `netdev e1000e`; falls back to the active
 * driver. */
const struct netdev *netdev_mgmt(void);

/* Select a NIC by short driver name ("e1000e", "r8168", "ne2000", "asix").
 * Returns 0 on success; leaves the current driver active on failure. */
int netdev_select(const char *name);
void program_netdev_main(const char *arg);

/* Busy gate for commands that poll the NIC for a while (netrecv, ping,
 * netloop, netkexec): the background kexec watcher must not steal
 * frames while one of them owns the receiver. */
void netdev_acquire(void);
void netdev_release(void);
int  netdev_in_use(void);

/* Process that currently owns the NIC (0 = free), and a hard reset used by
 * the reaper so a killed process cannot leave the receiver owned. */
unsigned int netdev_owner(void);
void netdev_release_all(void);

/* Fill the active NIC's MAC; -1 (and a zero MAC) when there is no NIC. */
int netdev_get_mac(unsigned char mac[6]);

/* One shared frame-receive scratch buffer (1518 bytes) for protocol code that
 * just needs somewhere for nd->rx() to land.  Only valid while the caller
 * owns the NIC, and only until the next rx().  Shared so the DNS/TCP/ARP
 * paths don't each carry their own frame buffer in the kernel's BSS. */
unsigned char *netdev_rx_scratch(void);
#define NETDEV_FRAME_MAX 1518

#endif
