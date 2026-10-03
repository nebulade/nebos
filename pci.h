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

static inline void port_outw(unsigned short port, unsigned short v)
{
	__asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned short port_inw(unsigned short port)
{
	unsigned short v;
	__asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline unsigned int pci_cfg_read32(unsigned int bus, unsigned int dev,
					  unsigned int func, unsigned int reg)
{
	port_outl(0xCF8, 0x80000000 | (bus << 16) | (dev << 11) |
		  (func << 8) | (reg & 0xFC));
	return port_inl(0xCFC);
}

static inline void pci_cfg_write32(unsigned int bus, unsigned int dev,
				   unsigned int func, unsigned int reg,
				   unsigned int val)
{
	port_outl(0xCF8, 0x80000000 | (bus << 16) | (dev << 11) |
		  (func << 8) | (reg & 0xFC));
	port_outl(0xCFC, val);
}

static inline void pci_cfg_write16(unsigned int bus, unsigned int dev,
				   unsigned int func, unsigned int reg,
				   unsigned short val)
{
	port_outl(0xCF8, 0x80000000 | (bus << 16) | (dev << 11) |
		  (func << 8) | (reg & 0xFC));
	port_outw((unsigned short)(0xCFC + (reg & 2)), val);
}

static inline void pci_cfg_write8(unsigned int bus, unsigned int dev,
				  unsigned int func, unsigned int reg,
				  unsigned char val)
{
	port_outl(0xCF8, 0x80000000 | (bus << 16) | (dev << 11) |
		  (func << 8) | (reg & 0xFC));
	port_outb((unsigned short)(0xCFC + (reg & 3)), val);
}

#endif
