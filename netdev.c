/* NIC driver registry: see netdev.h. */

#include "netdev.h"
#include "asix.h"
#include "e1000e.h"
#include "io.h"
#include "ne2000.h"
#include "net.h"
#include "process.h"
#include "r8168.h"
static const struct netdev *const netdev_drivers[] = {
	/* e1000e is the active test NIC; the ASIX is kept as the management
	 * link by netdev_mgmt() even while e1000e is active. */
	&e1000e_netdev,
	&r8168_netdev,
	&ne2000_netdev,
	&asix_netdev,
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

/* Apply the driver's link-local/static default when it becomes active.
 * The ASIX direct link uses 192.168.50.2/24; the e1000e keeps the router
 * subnet at 192.168.178.240. */
static void netdev_apply_defaults(const struct netdev *nd)
{
	if (nd && nd->default_ip)
		net_configure(nd->default_ip, nd->default_mask, 0);
}

const struct netdev *netdev_active(void)
{
	unsigned int i;

	if (!probed) {
		probed = 1;
		for (i = 0; i < sizeof(netdev_drivers) / sizeof(netdev_drivers[0]); i++) {
			if (netdev_drivers[i]->init() == 0) {
				active = netdev_drivers[i];
				netdev_apply_defaults(active);
				break;
			}
		}
	}
	return active;
}

/* The ASIX dongle is the direct management link when present.  Keeping
 * netlog/netkexec here lets the active driver be switched to e1000e for
 * testing without losing the netmon console or `make net-dev`. */
const struct netdev *netdev_mgmt(void)
{
	if (asix_netdev.init() == 0)
		return &asix_netdev;
	return netdev_active();
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

static int name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

int netdev_select(const char *name)
{
	unsigned int i;

	if (!name || !*name || busy)
		return -1;
	while (*name == ' ')
		name++;
	for (i = 0; i < sizeof(netdev_drivers) / sizeof(netdev_drivers[0]);
	     i++) {
		if (!name_eq(netdev_drivers[i]->name, name))
			continue;
		if (active == netdev_drivers[i]) {
			probed = 1;
			return 0;
		}
		if (netdev_drivers[i]->init() != 0)
			return -1;
		active = netdev_drivers[i];
		probed = 1;
		netdev_apply_defaults(active);
		return 0;
	}
	return -1;
}

static void print_netdev(const char *label, const struct netdev *nd)
{
	io_putstr("netdev: ");
	io_putstr(label);
	io_putstr(nd->name);
	io_putstr(" (");
	io_putstr(nd->chip);
	io_putstr(")\n");
}

void program_netdev_main(const char *arg)
{
	const struct netdev *mgmt;

	if (arg && *arg) {
		while (*arg == ' ')
			arg++;
		if (netdev_select(arg) != 0) {
			io_putstr("netdev: cannot select '");
			io_putstr(arg);
			io_putstr("'\n");
			return;
		}
	} else if (!netdev_active()) {
		io_putstr("netdev: no NIC\n");
		return;
	}

	print_netdev("active ", netdev_active());
	if (netdev_active()->print_info)
		netdev_active()->print_info();
	mgmt = netdev_mgmt();
	if (mgmt && mgmt != netdev_active())
		print_netdev("mgmt   ", mgmt);
	if (!(arg && *arg))
		io_putstr("netdev: use netdev asix|e1000e|r8168|ne2000\n");
}
