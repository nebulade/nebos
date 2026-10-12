/* Push kernel.bin to a Nebs OS board over UDP (the board's WM loop
 * auto-detects the header and jumps into the new kernel).
 *
 *   netkexec_send <board-ip> <port> <entry-hex> <file>
 *
 * Protocol: see netkexec.c.  Stop-and-wait: every datagram (except the
 * header) is acked with the next expected offset; lost datagrams are
 * retransmitted, so a slow or busy board just slows the transfer down.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DATA_MAX   1400
#define MAGIC_HDR  0x554E584Bu   /* "KXNU" */
#define MAGIC_PAY   0x504E584Bu   /* "KXNP" */
#define MAGIC_DATA 0x444E584Bu   /* "KXND" */
#define MAGIC_ACK  0x414E584Bu   /* "KXNA" */
#define ACK_DONE   1

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static int send_packet(int fd, const struct sockaddr_in *dst,
		       const unsigned char *buf, size_t len)
{
	return sendto(fd, buf, len, 0, (const struct sockaddr *)dst,
		      sizeof(*dst)) == (ssize_t)len ? 0 : -1;
}

/* Wait for an ACK ("KXNA" next_offset status).  Returns next_offset, or -1
 * on timeout. */
static long wait_ack(int fd, unsigned int timeout_ms, uint32_t *status)
{
	struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
	unsigned char buf[512];
	uint32_t next_offset;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);

		if (n < 0)
			return -1;                 /* timeout */
		if (n < 12 || rd32(buf) != MAGIC_ACK)
			continue;                  /* not ours */
		next_offset = rd32(buf + 4);
		*status = rd32(buf + 8);
		return (long)next_offset;
	}
}

int main(int argc, char **argv)
{
	const char *ip, *path;
	unsigned int port;
	unsigned int entry;
	unsigned char *data, *pkt;
	long size;
	FILE *f;
	unsigned int offset = 0, sum = 0;
	int fd, i, fails = 0;
	int payload = 0;
	struct sockaddr_in dst;

	/* --payload: replace the command pack (/ramfs) instead of the kernel. */
	if (argc > 1 && strcmp(argv[1], "--payload") == 0) {
		if (argc != 5) {
			fprintf(stderr, "usage: %s --payload <board-ip> <port> <file>\n",
				argv[0]);
			return 2;
		}
		payload = 1;
		ip = argv[2];
		port = (unsigned int)strtoul(argv[3], NULL, 0);
		path = argv[4];
		entry = 0;
	} else {
		if (argc != 5) {
			fprintf(stderr,
				"usage: %s <board-ip> <port> <entry-hex> <file>\n",
				argv[0]);
			return 2;
		}
		ip = argv[1];
		port = (unsigned int)strtoul(argv[2], NULL, 0);
		entry = (unsigned int)strtoul(argv[3], NULL, 16);
		path = argv[4];
	}

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "netkexec_send: %s: %s\n", path, strerror(errno));
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 0x400000) {   /* matches kexec.h KEXEC_MAX */
		fprintf(stderr, "netkexec_send: bad size %ld\n", size);
		fclose(f);
		return 1;
	}
	data = malloc((size_t)size);
	if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "netkexec_send: read failed\n");
		fclose(f);
		return 1;
	}
	fclose(f);
	for (i = 0; i < size; i++)
		sum += data[i];

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
		fprintf(stderr, "netkexec_send: bad address %s\n", ip);
		return 1;
	}

	pkt = malloc(12 + DATA_MAX);
	if (payload)
		printf("netkexec: %s:%u, %ld bytes, command pack for /ramfs\n",
		       ip, port, size);
	else
		printf("netkexec: %s:%u, %ld bytes, entry 0x%08x\n",
		       ip, port, size, entry);

	/* Header first: the board answers with the first expected offset. */
	wr32(pkt, payload ? MAGIC_PAY : MAGIC_HDR);
	wr32(pkt + 4, entry);
	wr32(pkt + 8, (uint32_t)size);
	wr32(pkt + 12, (uint32_t)sum);
	for (i = 0; i < 20; i++) {
		uint32_t status;
		long ack;

		if (send_packet(fd, &dst, pkt, 16) != 0) {
			perror("sendto");
			return 1;
		}
		ack = wait_ack(fd, 500, &status);
		if (ack == 0)
			break;
		if (ack > 0) {
			fprintf(stderr, "netkexec: unexpected ack offset %ld\n", ack);
			return 1;
		}
	}
	if (i == 20) {
		fprintf(stderr, "netkexec: no ack from the board (is it running and idle?)\n");
		return 1;
	}

	/* Stop-and-wait data transfer. */
	while (offset < (unsigned int)size) {
		unsigned int chunk = (unsigned int)size - offset;
		uint32_t status = 0;
		long ack;

		if (chunk > DATA_MAX)
			chunk = DATA_MAX;
		wr32(pkt, MAGIC_DATA);
		wr32(pkt + 4, offset);
		wr32(pkt + 8, chunk);
		memcpy(pkt + 12, data + offset, chunk);

		if (send_packet(fd, &dst, pkt, 12 + chunk) != 0) {
			perror("sendto");
			return 1;
		}
		ack = wait_ack(fd, 500, &status);
		if (ack < 0) {
			if (++fails > 40) {
				fprintf(stderr, "\nnetkexec: giving up at offset %u\n", offset);
				return 1;
			}
			continue;                  /* retransmit */
		}
		fails = 0;
		if (status == ACK_DONE && (unsigned int)ack == (unsigned int)size) {
			offset = (unsigned int)ack;
			break;
		}
		if (ack > (long)offset && (unsigned int)ack <= (unsigned int)size)
			offset = (unsigned int)ack;
		printf("\rnetkexec: %3u%% (%u/%ld bytes)", offset * 100 / (unsigned)size,
		       offset, size);
		fflush(stdout);
	}
	if (payload)
		printf("\rnetkexec: 100%% (%ld bytes) - pack accepted, /ramfs replaced\n",
		       size);
	else
		printf("\rnetkexec: 100%% (%ld bytes) - image accepted, board is jumping\n",
		       size);
	close(fd);
	free(pkt);
	free(data);
	return 0;
}
