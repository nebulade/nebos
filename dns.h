/* Minimal DNS resolver (A records over UDP, RFC 1035): see dns.c. */
#ifndef DNS_H
#define DNS_H

/* Resolve `name` to an IPv4 address with a recursive query to the configured
 * resolver (DHCP option 6, else the gateway).  Prints a `dns:` line.
 * Returns 0 on success.  The caller must own the NIC (netdev_acquire). */
int dns_resolve(const char *name, unsigned char ip[4]);

/* `netdns [a.b.c.d]` (show/set the resolver) and `lookup <name>`. */
void program_netdns_main(const char *arg);
void program_lookup_main(const char *arg);

#endif
