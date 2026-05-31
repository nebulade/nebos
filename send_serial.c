/* Simple binary sender for nebos serial_recv.
   Protocol: receiver sends 'R', then sender sends:
     4 bytes: size (big-endian)
     N bytes: data
     1 byte: checksum (sum of data bytes mod 256)
   Usage: send_serial <device> <baud> <file> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdint.h>

int main(int argc, char *argv[])
{
	int fd;
	FILE *fp;
	unsigned char *data;
	long size;
	unsigned int cksum;
	unsigned char buf[5];
	unsigned char r;
	struct termios tty;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s <device> <baud> <file>\n", argv[0]);
		return 1;
	}

	fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* Configure serial port. */
	memset(&tty, 0, sizeof(tty));
	if (tcgetattr(fd, &tty) != 0) {
		perror("tcgetattr");
		return 1;
	}

	cfsetispeed(&tty, atoi(argv[2]));
	cfsetospeed(&tty, atoi(argv[2]));

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
		return 1;
	}

	/* Read file. */
	fp = fopen(argv[3], "rb");
	if (!fp) {
		perror("fopen");
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	data = malloc(size);
	if (!data) {
		perror("malloc");
		return 1;
	}
	if (fread(data, 1, size, fp) != (size_t)size) {
		perror("fread");
		return 1;
	}
	fclose(fp);

	/* Compute checksum. */
	cksum = 0;
	for (long i = 0; i < size; i++)
		cksum += data[i];
	cksum &= 0xFF;

	/* Wait for 'R' from receiver. */
	fprintf(stderr, "Waiting for receiver...\n");
	if (read(fd, &r, 1) != 1 || r != 'R') {
		fprintf(stderr, "No 'R' received\n");
		return 1;
	}
	fprintf(stderr, "Got 'R', sending %ld bytes...\n", size);

	/* Send size (4 bytes big-endian). */
	buf[0] = (size >> 24) & 0xFF;
	buf[1] = (size >> 16) & 0xFF;
	buf[2] = (size >> 8) & 0xFF;
	buf[3] = size & 0xFF;
	if (write(fd, buf, 4) != 4) {
		perror("write size");
		return 1;
	}
	tcdrain(fd);

	/* Send data. */
	if (write(fd, data, size) != size) {
		perror("write data");
		return 1;
	}
	tcdrain(fd);

	/* Send checksum. */
	buf[0] = cksum;
	if (write(fd, buf, 1) != 1) {
		perror("write checksum");
		return 1;
	}
	tcdrain(fd);

	fprintf(stderr, "Done.\n");
	free(data);
	close(fd);
	return 0;
}
