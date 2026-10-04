/* Minimal network stack: ARP + ICMP echo (ping) over the NE2000 driver.
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

const char *net_ethertype_name(unsigned short et);

/* Print one decoded frame summary line (dst/src/ethertype + ARP/IP/ICMP). */
void net_decode_frame(const unsigned char *frame, unsigned int len);

/* Build a reply to an ARP request or ICMP echo request addressed to us.
   Returns reply length (> 0) or 0 if the frame needs no reply. */
int net_build_reply(const unsigned char *frame, unsigned int len,
		    unsigned char *out, unsigned int maxout);

#endif
