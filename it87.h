/* Read-only ITE IT87xx Super I/O hardware monitor (AT3IONT-I). */
#ifndef IT87_H
#define IT87_H

#define IT87_TEMP_COUNT 3
#define IT87_FAN_COUNT  3

struct it87_info {
	int present;
	unsigned int chip_id;
	unsigned int base;
	unsigned int temp_enable;
	unsigned int temp_extra;
	int temp_c[IT87_TEMP_COUNT];
	unsigned int fan_rpm[IT87_FAN_COUNT];
};

void it87_read(struct it87_info *out);

#endif
