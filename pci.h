/* PCI configuration space and low-level port I/O helpers. */
#ifndef PCI_H
#define PCI_H

static inline void port_outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned char port_inb(unsigned short port)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void port_outl(unsigned short port, unsigned int v)
{
	__asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned int port_inl(unsigned short port)
{
	unsigned int v;
	__asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline unsigned int pci_cfg_read32(unsigned int bus, unsigned int dev,
					  unsigned int func, unsigned int reg)
{
	port_outl(0xCF8, 0x80000000 | (bus << 16) | (dev << 11) |
		  (func << 8) | (reg & 0xFC));
	return port_inl(0xCFC);
}

#endif
