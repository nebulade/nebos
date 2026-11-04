/* ASIX AX88772 USB 2.0 Ethernet driver.
 *
 * The AX88772 is a vendor-specific USB device with one bulk IN and one bulk
 * OUT endpoint.  Configuration, the embedded PHY and the MAC are reached with
 * the ASIX vendor control requests; the receive path prepends a 4-byte header
 * (length in the low 16 bits and its complement in the high 16 bits), the
 * transmit path expects the same header before the Ethernet frame.
 *
 * The USB core's bulk API is start-or-poll: usb_bulk_in() arms on the first
 * call and leaves the transfer disarmed after delivering a chunk, so the
 * RX buffer stays stable while its (possibly several) frames are consumed. */

#include "asix.h"
#include "io.h"
#include "timer.h"
#include "usb.h"

/* ASIX vendor requests. */
#define AX_CMD_SET_SW_MII	0x06
#define AX_CMD_READ_MII_REG	0x07
#define AX_CMD_WRITE_MII_REG	0x08
#define AX_CMD_SET_HW_MII	0x0A
#define AX_CMD_READ_RX_CTL	0x0F
#define AX_CMD_WRITE_RX_CTL	0x10
#define AX_CMD_WRITE_IPG0	0x12
#define AX_CMD_READ_NODE_ID	0x13
#define AX_CMD_WRITE_NODE_ID	0x14
#define AX_CMD_READ_PHY_ID	0x19
#define AX_CMD_WRITE_MEDIUM_MODE 0x1B
#define AX_CMD_WRITE_GPIOS	0x1F
#define AX_CMD_SW_RESET		0x20
#define AX_CMD_SW_PHY_SELECT	0x22

#define AX_GPIO_RSE		0x80
#define AX_GPIO_GPO_2		0x20
#define AX_GPIO_GPO2EN		0x10

#define AX_SWRESET_CLEAR	0x00
#define AX_SWRESET_PRTE		0x04
#define AX_SWRESET_PRL		0x08
#define AX_SWRESET_IPRL		0x20
#define AX_SWRESET_IPPD		0x40

#define AX88772_IPG0_DEFAULT	0x15
#define AX88772_IPG1_DEFAULT	0x0C
#define AX88772_IPG2_DEFAULT	0x12

#define AX_MEDIUM_FD		0x0002
#define AX_MEDIUM_AC		0x0004
#define AX_MEDIUM_RFC		0x0010
#define AX_MEDIUM_TFC		0x0020
#define AX_MEDIUM_RE		0x0100
#define AX_MEDIUM_PS		0x0200
#define AX88772_MEDIUM_DEFAULT	(AX_MEDIUM_FD | AX_MEDIUM_RFC | \
				 AX_MEDIUM_TFC | AX_MEDIUM_PS | \
				 AX_MEDIUM_AC | AX_MEDIUM_RE)

#define AX_RX_CTL_SO		0x0080
#define AX_RX_CTL_AB		0x0008
#define AX_DEFAULT_RX_CTL	(AX_RX_CTL_SO | AX_RX_CTL_AB)

#define MII_BMCR		0
#define MII_BMSR		1
#define MII_ADVERTISE		4
#define BMCR_RESET		0x8000
#define BMCR_ANENABLE		0x1000
#define BMCR_ANRESTART		0x0200
#define BMSR_LSTATUS		0x0004
#define ADVERTISE_ALL		0x01E0
#define ADVERTISE_CSMA		0x0001

#define ASIX_RX_MAX		2048
#define ASIX_TX_MAX		(1514 + 4)


struct asix_dev {
	int present;
	int chip_ready;
	int link;
	unsigned int ctrl;
	unsigned int addr;
	unsigned int mps;
	unsigned int port;
	int low;
	unsigned int ep_in, ep_in_mps;
	unsigned int ep_out, ep_out_mps;
	unsigned int phy;
	unsigned char mac[6];
};

static struct asix_dev dev;
static unsigned char rx_buf[ASIX_RX_MAX] __attribute__((aligned(64)));
static unsigned char tx_buf[ASIX_TX_MAX] __attribute__((aligned(64)));
static unsigned int rx_pos, rx_end;



static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

static int read_cmd(unsigned char cmd, unsigned short value,
		    unsigned short index, void *buf, unsigned int len)
{
	int n = usb_control_in(dev.ctrl, dev.addr, dev.mps, dev.low,
			       0xC0, cmd, value, index, (unsigned char *)buf,
			       len);
	return n == (int)len ? 0 : -1;
}

static int write_cmd(unsigned char cmd, unsigned short value,
		     unsigned short index, const void *buf, unsigned int len)
{
	if (len) {
		int n = usb_control_out_data(dev.ctrl, dev.addr, dev.mps,
					     dev.low, 0x40, cmd, value, index,
					     (const unsigned char *)buf, len);
		return n < 0 ? -1 : 0;
	}
	return usb_control_out(dev.ctrl, dev.addr, dev.mps, dev.low,
			       0x40, cmd, value, index) < 0 ? -1 : 0;
}

static int mdio_read(unsigned int loc)
{
	unsigned char b[2];

	if (write_cmd(AX_CMD_SET_SW_MII, 0, 0, 0, 0) < 0)
		return -1;
	if (read_cmd(AX_CMD_READ_MII_REG, (unsigned short)dev.phy,
		     (unsigned short)loc, b, 2) < 0) {
		write_cmd(AX_CMD_SET_HW_MII, 0, 0, 0, 0);
		return -1;
	}
	write_cmd(AX_CMD_SET_HW_MII, 0, 0, 0, 0);
	return (int)((unsigned int)b[0] | ((unsigned int)b[1] << 8));
}

static void mdio_write(unsigned int loc, unsigned int val)
{
	unsigned char b[2];

	b[0] = (unsigned char)(val & 0xFF);
	b[1] = (unsigned char)((val >> 8) & 0xFF);
	write_cmd(AX_CMD_SET_SW_MII, 0, 0, 0, 0);
	write_cmd(AX_CMD_WRITE_MII_REG, (unsigned short)dev.phy,
		  (unsigned short)loc, b, 2);
	write_cmd(AX_CMD_SET_HW_MII, 0, 0, 0, 0);
}

static int chip_init(void)
{
	unsigned char b[2];
	unsigned int phy, rx_ctl, i;
	int bmcr;

	dev.chip_ready = 0;
	dev.link = 0;

	if (write_cmd(AX_CMD_WRITE_GPIOS,
		      AX_GPIO_RSE | AX_GPIO_GPO_2 | AX_GPIO_GPO2EN,
		      0, 0, 0) < 0)
		return -1;
	delay_ms(5);

	if (read_cmd(AX_CMD_READ_PHY_ID, 0, 0, b, 2) < 0)
		return -1;
	phy = b[1];
	dev.phy = phy;

	if (write_cmd(AX_CMD_SW_PHY_SELECT,
		      (phy & 0x1F) == 0x10 ? 1 : 0, 0, 0, 0) < 0)
		return -1;

	if (write_cmd(AX_CMD_SW_RESET, AX_SWRESET_IPPD | AX_SWRESET_PRL,
		      0, 0, 0) < 0)
		return -1;
	delay_ms(150);
	if (write_cmd(AX_CMD_SW_RESET, AX_SWRESET_CLEAR, 0, 0, 0) < 0)
		return -1;
	delay_ms(150);
	if ((phy & 0x1F) == 0x10)
		write_cmd(AX_CMD_SW_RESET, AX_SWRESET_IPRL, 0, 0, 0);
	else
		write_cmd(AX_CMD_SW_RESET, AX_SWRESET_PRTE, 0, 0, 0);
	delay_ms(150);

	if (read_cmd(AX_CMD_READ_RX_CTL, 0, 0, b, 2) < 0)
		return -1;
	rx_ctl = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
	(void)rx_ctl;
	if (write_cmd(AX_CMD_WRITE_RX_CTL, 0, 0, 0, 0) < 0)
		return -1;

	if (read_cmd(AX_CMD_READ_NODE_ID, 0, 0, dev.mac, 6) < 0)
		return -1;

	/* PHY reset and 10/100 autonegotiation. */
	mdio_write(MII_BMCR, BMCR_RESET);
	delay_ms(2);
	mdio_write(MII_ADVERTISE, ADVERTISE_ALL | ADVERTISE_CSMA);
	bmcr = mdio_read(MII_BMCR);
	if (bmcr >= 0 && (bmcr & BMCR_ANENABLE))
		mdio_write(MII_BMCR, (unsigned int)bmcr | BMCR_ANRESTART);

	if (write_cmd(AX_CMD_WRITE_MEDIUM_MODE, AX88772_MEDIUM_DEFAULT,
		      0, 0, 0) < 0)
		return -1;
	if (write_cmd(AX_CMD_WRITE_IPG0,
		      AX88772_IPG0_DEFAULT | AX88772_IPG1_DEFAULT,
		      AX88772_IPG2_DEFAULT, 0, 0) < 0)
		return -1;
	if (write_cmd(AX_CMD_WRITE_RX_CTL, AX_DEFAULT_RX_CTL,
		      0, 0, 0) < 0)
		return -1;
	if (write_cmd(AX_CMD_WRITE_NODE_ID, 0, 0, dev.mac, 6) < 0)
		return -1;

	/* Wait for the PHY link, but do not fail the NIC if no cable is
	 * plugged in yet: dhcp/netinfo can retry later. */
	for (i = 0; i < 60; i++) {
		int st = mdio_read(MII_BMSR);
		if (st >= 0 && (st & BMSR_LSTATUS)) {
			dev.link = 1;
			break;
		}
		delay_ms(50);
	}

	dev.chip_ready = 1;
	return 0;
}

static void put_mac(void)
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		static const char hex[] = "0123456789ABCDEF";
		if (i)
			io_putchar(':');
		io_putchar(hex[(dev.mac[i] >> 4) & 0xF]);
		io_putchar(hex[dev.mac[i] & 0xF]);
	}
}

static int net_init(void)
{
	if (!dev.present)
		return -1;
	if (!dev.chip_ready && chip_init() < 0)
		return -1;
	return 0;
}

static void net_get_mac(unsigned char mac[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		mac[i] = dev.mac[i];
}

static int net_rx(unsigned char *buf, unsigned int maxlen)
{
	unsigned int hdr, len, i;
	int n;

	if (!dev.present)
		return -1;

	for (;;) {
		if (rx_pos < rx_end) {
			if (rx_pos + 4 > rx_end) {
				rx_pos = rx_end;
				return -1;
			}
			hdr = (unsigned int)rx_buf[rx_pos] |
			      ((unsigned int)rx_buf[rx_pos + 1] << 8) |
			      ((unsigned int)rx_buf[rx_pos + 2] << 16) |
			      ((unsigned int)rx_buf[rx_pos + 3] << 24);
			len = hdr & 0x7FF;
			if (((~hdr >> 16) & 0x7FF) != len ||
			    len < 14 || len > maxlen ||
			    rx_pos + 4 + len > rx_end) {
				rx_pos = rx_end;
				return -1;
			}
			for (i = 0; i < len; i++)
				buf[i] = rx_buf[rx_pos + 4 + i];
			rx_pos += 4 + ((len + 1) & ~1u);
			return (int)len;
		}

		n = usb_bulk_in(dev.ctrl, dev.addr, dev.ep_in,
				dev.ep_in_mps, rx_buf, ASIX_RX_MAX);
		if (n <= 0)
			return -1;
		rx_pos = 0;
		rx_end = (unsigned int)n;
	}
}

static int net_tx(const unsigned char *buf, unsigned int len)
{
	unsigned int hdr;

	if (!dev.present || len > 1514)
		return -1;
	hdr = len | ((~len & 0xFFFF) << 16);
	tx_buf[0] = (unsigned char)(hdr & 0xFF);
	tx_buf[1] = (unsigned char)((hdr >> 8) & 0xFF);
	tx_buf[2] = (unsigned char)((hdr >> 16) & 0xFF);
	tx_buf[3] = (unsigned char)((hdr >> 24) & 0xFF);
	{
		unsigned int i;
		for (i = 0; i < len; i++)
			tx_buf[4 + i] = buf[i];
	}
	return usb_bulk_out(dev.ctrl, dev.addr, dev.ep_out,
			    dev.ep_out_mps, tx_buf, len + 4) == 0 ? 0 : -1;
}

static void net_print_info(void)
{
	int st;

	io_putstr("ASIX AX88772 USB 2.0 Ethernet\n");
	io_putstr("  MAC ");
	put_mac();
	io_putstr("\n");
	st = dev.phy ? mdio_read(MII_BMSR) : -1;
	if (st >= 0 && (st & BMSR_LSTATUS))
		io_putstr("  link up, 100 Mbit capable\n");
	else
		io_putstr("  link down\n");
}

static void net_reset(void)
{
	rx_pos = 0;
	rx_end = 0;
	dev.chip_ready = 0;
	if (dev.present)
		chip_init();
}

static const unsigned char asix_default_ip[4] = { 192, 168, 50, 2 };
static const unsigned char asix_default_mask[4] = { 255, 255, 255, 0 };

const struct netdev asix_netdev = {
	"asix",
	"AX88772 USB 2.0 Ethernet",
	net_init,
	net_get_mac,
	net_rx,
	net_tx,
	net_print_info,
	0,
	0,
	net_reset,
	0,
	0,
	asix_default_ip,
	asix_default_mask,
	0,
	0,
};

/* Read and parse the configuration descriptor.  The AX88772 has one
 * vendor-specific interface with a bulk IN and a bulk OUT endpoint. */
static int asix_config(void)
{
	unsigned char cfg[128];
	unsigned int total, i, config_value = 0;
	unsigned int ep_in = 0, ep_out = 0, ep_in_mps = 0, ep_out_mps = 0;

	if (usb_control_in(dev.ctrl, dev.addr, dev.mps, dev.low,
			   0x80, 0x06, 0x0200, 0, cfg, 9) < 0)
		return -1;
	total = (unsigned int)cfg[2] | ((unsigned int)cfg[3] << 8);
	if (total < 9)
		return -1;
	if (total > sizeof(cfg))
		total = sizeof(cfg);
	if (usb_control_in(dev.ctrl, dev.addr, dev.mps, dev.low,
			   0x80, 0x06, 0x0200, 0, cfg, total) < 0)
		return -1;

	config_value = cfg[5];
	i = 0;
	while (i + 2 <= total) {
		unsigned int blen = cfg[i];
		unsigned int btype = cfg[i + 1];

		if (blen < 2 || i + blen > total)
			break;
		if (btype == 0x05 && blen >= 7) {
			unsigned int epa = cfg[i + 2];
			unsigned int attr = cfg[i + 3] & 3u;
			unsigned int mps = ((unsigned int)cfg[i + 4] |
					    ((unsigned int)cfg[i + 5] << 8)) &
					   0x7FFu;

			if (attr == 2) {
				if (epa & 0x80) {
					ep_in = epa & 0x0F;
					ep_in_mps = mps;
				} else {
					ep_out = epa & 0x0F;
					ep_out_mps = mps;
				}
			}
		}
		i += blen;
	}

	if (!ep_in || !ep_out || !config_value)
		return -1;
	if (usb_control_out(dev.ctrl, dev.addr, dev.mps, dev.low,
			    0x00, 0x09, (unsigned short)config_value, 0) < 0)
		return -1;

	dev.ep_in = ep_in;
	dev.ep_in_mps = ep_in_mps ? ep_in_mps : 64;
	dev.ep_out = ep_out;
	dev.ep_out_mps = ep_out_mps ? ep_out_mps : 64;
	return 0;
}

static int device_probe(unsigned int ctrl, unsigned int addr,
			unsigned int mps, int low_speed, unsigned int port,
			unsigned int vendor, unsigned int product)
{
	if (vendor != 0x0B95 || (product != 0x7720 && product != 0x772A &&
				 product != 0x772B))
		return 0;

	dev.ctrl = ctrl;
	dev.addr = addr;
	dev.mps = mps;
	dev.low = low_speed;
	dev.port = port;
	dev.present = 1;
	dev.chip_ready = 0;
	rx_pos = rx_end = 0;

	if (asix_config() < 0) {
		dev.present = 0;
		return 0;
	}
	if (chip_init() < 0) {
		dev.present = 0;
		return 0;
	}

	return 1;
}

void asix_register(void)
{
	usb_register_device_probe(device_probe);
}
