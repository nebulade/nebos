/* Network services for ring-3 commands (see sys_net.c). */
#ifndef SYS_NET_H
#define SYS_NET_H

#include "syscall.h"

/* Release the receiver if `pid` owned it (called by the reaper). */
void sys_net_reap(unsigned int pid);

int sys_net_info(struct sys_netinfo *out);
int sys_net_print_info(void);
int sys_net_acquire(void);
int sys_net_release(void);
int sys_net_mac(unsigned char mac[6]);
void sys_net_get_ip(unsigned char ip[4]);
void sys_net_set_ip(const unsigned char ip[4]);
void sys_net_get_mask(unsigned char mask[4]);
void sys_net_get_gw(unsigned char gw[4]);
void sys_net_get_dns(unsigned char dns[4]);
void sys_net_set_dns(const unsigned char dns[4]);
int sys_net_tx(const unsigned char *buf, unsigned int len);
int sys_net_rx(unsigned char *buf, unsigned int max);
void sys_net_decode(const unsigned char *buf, unsigned int len);
int sys_net_reply(const unsigned char *in, unsigned int len,
		  unsigned char *out);
int sys_net_loopback(int on);
int sys_net_regs(void);
int sys_net_reset(void);
int sys_dhcp_run(unsigned int timeout_ms);
int sys_dhcp_info(unsigned char server[4], unsigned int *lease);
int sys_dns_resolve(const char *name, unsigned char ip[4]);
int sys_netlog(const char *text);
int sys_netlog_conf(int enabled, const unsigned char *ip);
int sys_tcp_connect(const unsigned char mac[6], const unsigned char ip[4],
		    unsigned int port);
int sys_tcp_peer_mss(void);
int sys_tcp_send(const void *buf, unsigned int len);
int sys_tcp_recv(void *buf, unsigned int max, unsigned int timeout_ms);
void sys_tcp_close(void);
int sys_net_next_hop(const unsigned char dst[4], unsigned char hop[4]);
int sys_net_arp(const unsigned char ip[4], unsigned char mac[6]);
int sys_net_configured(void);

#endif
