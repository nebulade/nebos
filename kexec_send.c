/* Host-side sender for nebos `kexec` / bootstrap.
 *
 * Protocol: send immediately (no handshake; the kernel auto-detects the magic):
 *   4 bytes: magic "KXEC" (LE)
 *   4 bytes: entry address (LE)
 *   4 bytes: size (LE)
 *   N bytes: kernel.bin data
 *   1 byte:  checksum (sum of data bytes mod 256)
 *
 * Usage: kexec_send <device> <baud> <entry_hex> <file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdint.h>

static void put32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

int main(int argc, char *argv[])
{
	int fd;
	FILE *fp;
	unsigned char *data;
	long size;
	unsigned int cksum = 0;
	unsigned char hdr[12];
	struct termios tty;
	uint32_t entry;
	const char *dev, *baud, *file;
	int payload = 0;

	if (argc > 1 && strcmp(argv[1], "--payload") == 0) {
		if (argc != 5) {
			fprintf(stderr, "Usage: %s --payload <device> <baud> <file>\n",
				argv[0]);
			return 1;
		}
		payload = 1;
		dev = argv[2];
		baud = argv[3];
		file = argv[4];
		entry = 0;
	} else {
		if (argc != 5) {
			fprintf(stderr, "Usage: %s <device> <baud> <entry_hex> <file>\n",
				argv[0]);
			return 1;
		}
		dev = argv[1];
		baud = argv[2];
		entry = (uint32_t)strtoul(argv[3], NULL, 16);
		file = argv[4];
	}

	fp = fopen(file, "rb");
	if (!fp) {
		perror("fopen");
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	data = malloc((size_t)size);
	if (!data) {
		perror("malloc");
		fclose(fp);
		return 1;
	}
	if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
		perror("fread");
		fclose(fp);
		free(data);
		return 1;
	}
	fclose(fp);

	for (long i = 0; i < size; i++)
		cksum += data[i];
	cksum &= 0xFF;

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open");
		free(data);
		return 1;
	}

	memset(&tty, 0, sizeof(tty));
	if (tcgetattr(fd, &tty) != 0) {
		perror("tcgetattr");
		free(data);
		close(fd);
		return 1;
	}
	cfsetispeed(&tty, (speed_t)atoi(baud));
	cfsetospeed(&tty, (speed_t)atoi(baud));
	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
	tty.c_cflag |= CLOCAL | CREAD;
	tty.c_cflag &= ~(PARENB | PARODD | CRTSCTS | CSTOPB);
	tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | ICRNL | INLCR);
	tty.c_oflag &= ~OPOST;
	tty.c_lflag = 0;
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		perror("tcsetattr");
		free(data);
		close(fd);
		return 1;
	}

	if (payload)
		fprintf(stderr, "Sending %ld bytes (command pack)...\n", size);
	else
		fprintf(stderr, "Sending %ld bytes (entry 0x%08x)...\n", size, entry);

	memcpy(hdr, payload ? "KXNP" : "KXEC", 4);
	put32le(hdr + 4, entry);
	put32le(hdr + 8, (uint32_t)size);

	if (write(fd, hdr, 12) != 12) {
		perror("write header");
		free(data);
		close(fd);
		return 1;
	}
	tcdrain(fd);

	if (write(fd, data, (size_t)size) != size) {
		perror("write data");
		free(data);
		close(fd);
		return 1;
	}
	tcdrain(fd);

	hdr[0] = (unsigned char)cksum;
	if (write(fd, hdr, 1) != 1) {
		perror("write checksum");
		free(data);
		close(fd);
		return 1;
	}
	tcdrain(fd);

	fprintf(stderr, "Done.\n");
	free(data);
	close(fd);
	return 0;
}
