/* Minimal network stack: ARP + ICMP echo (ping) over a netdev NIC.
   Parsing, decoding, reply generation, and the card's IPv4 address. */
#ifndef NET_H
#define NET_H

/* Ethernet ethertypes. */
#define ET_IPV4 0x0800
#define ET_ARP  0x0806
#define ET_IPV6 0x86DD

/* IPv4 protocol numbers. */
#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17
#define IPPROTO_TCP  6

/* ICMP message types. */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

void net_set_ip(const unsigned char ip[4]);
void net_get_ip(unsigned char ip[4]);
int  net_parse_ip(const char *s, unsigned char ip[4]);  /* 0 on success, -1 on error */
void net_print_ip(void);

/* Full configuration (mask/gateway stay zero when unknown).  net_set_ip()
 * keeps the manual override semantics and clears mask/gateway; DHCP uses
 * net_configure(). */
void net_configure(const unsigned char ip[4], const unsigned char mask[4],
		   const unsigned char gw[4]);
void net_get_mask(unsigned char mask[4]);
void net_get_gateway(unsigned char gw[4]);
int  net_is_configured(void);   /* ip != 0.0.0.0 */

/* DNS resolver address (DHCP option 6 or the `netdns` command); zero when
 * unknown.  Kept across net_set_ip()/net_configure() so a manual setting
 * survives a dhcp run without option 6. */
void net_get_dns(unsigned char dns[4]);
void net_set_dns(const unsigned char dns[4]);

/* Next hop towards dst: the gateway when dst is off-subnet and a gateway is
 * known, else dst itself. */
void net_next_hop(const unsigned char dst[4], unsigned char hop[4]);

/* Resolve dst_ip's MAC by ARP (3 tries, 1 s apart), polling the active NIC.
 * Returns 0 on success, -1 when no reply arrived, -2 on transmit error.
 * The caller must own the NIC (netdev_acquire). */
int net_arp_resolve(const unsigned char *dst_ip, unsigned char mac[6]);

const char *net_ethertype_name(unsigned short et);

/* Internet checksum (16-bit one's complement, network byte order). */
unsigned short net_checksum(const void *data, unsigned int len);

/* TCP checksum over the IPv4 pseudo-header (src/dst/proto/length) plus the
 * TCP header and payload; result is in network byte order. */
unsigned short net_checksum_tcp(const unsigned char *src_ip,
				const unsigned char *dst_ip,
				const unsigned char *tcp, unsigned int len);

/* Largest UDP payload net_udp_send() accepts. */
#define NET_UDP_MAX 1400

/* Send one UDP datagram (IPv4, checksum disabled) in a single Ethernet frame
 * to dst_mac/dst_ip, with the configured source address.  Returns 0 on
 * success, -1 on error. */
int net_udp_send(const unsigned char *dst_mac, const unsigned char *dst_ip,
		 unsigned int src_port, unsigned int dst_port,
		 const void *payload, unsigned int len);

/* Same, but with an explicit source address (DHCP uses 0.0.0.0). */
int net_udp_send_from(const unsigned char *src_ip,
		      const unsigned char *dst_mac, const unsigned char *dst_ip,
		      unsigned int src_port, unsigned int dst_port,
		      const void *payload, unsigned int len);

/* Parse one Ethernet frame as a UDP datagram addressed to us or to the
 * broadcast address.  Fills the addresses/ports and the payload location.
 * Returns 1 on match. */
int net_udp_parse(const unsigned char *frame, unsigned int len,
		  unsigned char src_ip[4], unsigned char dst_ip[4],
		  unsigned int *src_port, unsigned int *dst_port,
		  const unsigned char **payload, unsigned int *plen);

/* Print one decoded frame summary line (dst/src/ethertype + ARP/IP/ICMP). */
void net_decode_frame(const unsigned char *frame, unsigned int len);

/* Build a reply to an ARP request or ICMP echo request addressed to us.
   Returns reply length (> 0) or 0 if the frame needs no reply. */
int net_build_reply(const unsigned char *frame, unsigned int len,
		    unsigned char *out, unsigned int maxout);

#endif
