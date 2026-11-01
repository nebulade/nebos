/* Host-side sender for nebos `kexec` / `bootstrap` / `cmds-dev`.
 *
 * Kernel + rootfs protocol (serial, no handshake; the kernel auto-detects it):
 *   4 bytes: magic "KXE2" (LE)
 *   4 bytes: entry address (LE)
 *   4 bytes: kernel_size (LE)
 *   4 bytes: pack_size (LE)      -- 0 keeps the running rootfs
 *   N bytes: kernel.bin
 *   M bytes: rootfs pack (cmds_pack_gz.bin; may be omitted)
 *   1 byte:  checksum (sum of both payloads mod 256)
 *
 * Runtime pack replacement (`--payload`) keeps the old 12-byte framing:
 *   "KXNP", 0, pack_size, pack data, checksum.
 *
 * Usage: kexec_send <device> <baud> <entry_hex> <kernel> [pack]
 *        kexec_send --payload <device> <baud> <file>
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

static unsigned char *read_file(const char *path, long *size)
{
	FILE *fp = fopen(path, "rb");
	unsigned char *data;
	long n;

	if (!fp) {
		perror(path);
		exit(1);
	}
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n <= 0) {
		fprintf(stderr, "%s: empty file\n", path);
		fclose(fp);
		exit(1);
	}
	data = malloc((size_t)n);
	if (!data || fread(data, 1, (size_t)n, fp) != (size_t)n) {
		fprintf(stderr, "%s: read failed\n", path);
		fclose(fp);
		free(data);
		exit(1);
	}
	fclose(fp);
	*size = n;
	return data;
}

int main(int argc, char *argv[])
{
	int fd;
	unsigned char *kdata, *pdata = 0;
	long ksize, psize = 0;
	unsigned int cksum = 0;
	unsigned char hdr[16];
	struct termios tty;
	uint32_t entry;
	const char *dev, *baud, *file, *pack_file = 0;
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
		if (argc != 5 && argc != 6) {
			fprintf(stderr, "Usage: %s <device> <baud> <entry_hex> "
				"<kernel> [pack]\n", argv[0]);
			return 1;
		}
		dev = argv[1];
		baud = argv[2];
		entry = (uint32_t)strtoul(argv[3], NULL, 16);
		file = argv[4];
		if (argc == 6)
			pack_file = argv[5];
	}

	kdata = read_file(file, &ksize);
	if (pack_file)
		pdata = read_file(pack_file, &psize);

	for (long i = 0; i < ksize; i++)
		cksum += kdata[i];
	for (long i = 0; i < psize; i++)
		cksum += pdata[i];
	cksum &= 0xFF;

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open");
		if (pdata)
			free(pdata);
		free(kdata);
		return 1;
	}

	memset(&tty, 0, sizeof(tty));
	if (tcgetattr(fd, &tty) != 0) {
		perror("tcgetattr");
		if (pdata)
			free(pdata);
		free(kdata);
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
		if (pdata)
			free(pdata);
		free(kdata);
		close(fd);
		return 1;
	}

	if (payload)
		fprintf(stderr, "Sending %ld bytes (command pack)...\n", ksize);
	else
		fprintf(stderr, "Sending kernel %ld + rootfs %ld bytes "
			"(entry 0x%08x)...\n", ksize, psize, entry);

	if (payload) {
		memcpy(hdr, "KXNP", 4);
		put32le(hdr + 4, 0);
		put32le(hdr + 8, (uint32_t)ksize);
		if (write(fd, hdr, 12) != 12) {
			perror("write header");
			goto fail;
		}
	} else {
		memcpy(hdr, "KXE2", 4);
		put32le(hdr + 4, entry);
		put32le(hdr + 8, (uint32_t)ksize);
		put32le(hdr + 12, (uint32_t)psize);
		if (write(fd, hdr, 16) != 16) {
			perror("write header");
			goto fail;
		}
	}
	tcdrain(fd);

	if (write(fd, kdata, (size_t)ksize) != ksize) {
		perror("write kernel");
		goto fail;
	}
	if (pdata && write(fd, pdata, (size_t)psize) != psize) {
		perror("write rootfs");
		goto fail;
	}
	tcdrain(fd);

	hdr[0] = (unsigned char)cksum;
	if (write(fd, hdr, 1) != 1) {
		perror("write checksum");
		goto fail;
	}
	tcdrain(fd);

	fprintf(stderr, "Done.\n");
	if (pdata)
		free(pdata);
	free(kdata);
	close(fd);
	return 0;

fail:
	if (pdata)
		free(pdata);
	free(kdata);
	close(fd);
	return 1;
}
