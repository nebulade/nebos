/* Minimal DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK over
 * UDP broadcast, so a board with a NIC gets an address at boot.
 *
 * Runs before the WM loop (bounded to a few seconds) and on demand with the
 * `dhcp` command.  On failure the previously configured address is kept (the
 * static default), so nothing depends on a DHCP server being present. */
#ifndef DHCP_H
#define DHCP_H

/* Run the client with an overall time budget.  Returns:
 *   0  = an address was obtained and applied (net_configure),
 *  -1  = no server answered (configuration unchanged),
 *  -2  = protocol error / no NIC. */
int dhcp_run(unsigned int timeout_ms);

/* Info from the last successful run (server id + lease seconds).  Returns 1
 * if a lease has been obtained. */
int dhcp_last_info(unsigned char server[4], unsigned int *lease_s);

#endif
