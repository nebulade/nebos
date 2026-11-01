/* NIC driver registry: see netdev.h. */

#include "netdev.h"
#include "ne2000.h"
#include "process.h"
#include "r8168.h"
static const struct netdev *const netdev_drivers[] = {
	/* PCIe Gigabit first: on the AT3IONT-I this is the real NIC, while
	 * the NE2000 only exists under QEMU/ISA-era hardware. */
	&r8168_netdev,
	&ne2000_netdev,
};

static const struct netdev *active;
static int probed;
static unsigned int busy;
static unsigned int owner_pid;   /* process that took the NIC (busy > 0) */

/* Shared receive scratch: see netdev.h.  One buffer, reused by every protocol
 * that polls frames (they never run at the same time: the NIC is owned by one
 * command via netdev_acquire()). */
static unsigned char rx_scratch[NETDEV_FRAME_MAX];

unsigned char *netdev_rx_scratch(void)
{
	return rx_scratch;
}

void netdev_acquire(void)
{
	if (busy == 0 && current_proc())
		owner_pid = current_proc()->pid;
	busy++;
}

void netdev_release(void)
{
	if (busy && --busy == 0)
		owner_pid = 0;
}

/* The process currently owning the NIC (0 when free), and a hard reset for
 * the reaper: a process killed while inside a net operation must not leave
 * the receiver owned forever (it would pause the kexec watcher). */
unsigned int netdev_owner(void)
{
	return owner_pid;
}

void netdev_release_all(void)
{
	busy = 0;
	owner_pid = 0;
}

int netdev_in_use(void)
{
	return busy != 0;
}

const struct netdev *netdev_active(void)
{
	unsigned int i;

	if (!probed) {
		probed = 1;
		for (i = 0; i < sizeof(netdev_drivers) / sizeof(netdev_drivers[0]); i++) {
			if (netdev_drivers[i]->init() == 0) {
				active = netdev_drivers[i];
				break;
			}
		}
	}
	return active;
}

int netdev_get_mac(unsigned char mac[6])
{
	const struct netdev *nd = netdev_active();
	unsigned int i;

	if (!nd) {
		for (i = 0; i < 6; i++)
			mac[i] = 0;
		return -1;
	}
	nd->get_mac(mac);
	return 0;
}
