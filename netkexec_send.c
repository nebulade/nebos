/* Push a kernel + rootfs pack to a NebOS board over UDP (the board's WM
 * loop auto-detects the header and jumps into the new kernel).
 *
 *   netkexec_send <board-ip> <port> <entry-hex> <kernel> [pack]
 *   netkexec_send --payload <board-ip> <port> <pack>
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
#define MAGIC_HDR  0x324E584Bu   /* "KXN2" (kernel + rootfs) */
#define MAGIC_PAY  0x504E584Bu   /* "KXNP" (runtime pack replacement) */
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

static unsigned char *read_file(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long n;

	if (!f) {
		fprintf(stderr, "netkexec_send: %s: %s\n", path, strerror(errno));
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fprintf(stderr, "netkexec_send: %s: empty file\n", path);
		fclose(f);
		exit(1);
	}
	data = malloc((size_t)n);
	if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "netkexec_send: %s: read failed\n", path);
		fclose(f);
		free(data);
		exit(1);
	}
	fclose(f);
	*size = n;
	return data;
}

int main(int argc, char **argv)
{
	const char *ip, *kernel_path, *pack_path = 0;
	unsigned int port, entry;
	unsigned char *kdata, *pdata = 0, *data, *pkt;
	long ksize, psize = 0, size;
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
		kernel_path = argv[4];
		entry = 0;
	} else {
		if (argc != 5 && argc != 6) {
			fprintf(stderr,
				"usage: %s <board-ip> <port> <entry-hex> "
				"<kernel> [pack]\n", argv[0]);
			return 2;
		}
		ip = argv[1];
		port = (unsigned int)strtoul(argv[2], NULL, 0);
		entry = (unsigned int)strtoul(argv[3], NULL, 16);
		kernel_path = argv[4];
		if (argc == 6)
			pack_path = argv[5];
	}

	kdata = read_file(kernel_path, &ksize);
	if (pack_path)
		pdata = read_file(pack_path, &psize);
	size = ksize + psize;
	if (size <= 0 || size > 0x400000) {   /* matches kexec.h KEXEC_MAX */
		fprintf(stderr, "netkexec_send: bad total size %ld\n", size);
		return 1;
	}
	data = malloc((size_t)size);
	if (!data) {
		fprintf(stderr, "netkexec_send: out of memory\n");
		return 1;
	}
	memcpy(data, kdata, (size_t)ksize);
	if (psize)
		memcpy(data + ksize, pdata, (size_t)psize);
	for (i = 0; i < size; i++)
		sum += data[i];

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	{
		int on = 1;

		/* A board with no working unicast RX (or no DHCP lease) can
		 * still receive broadcasts; 255.255.255.255 then pushes a
		 * debug kernel over the normal LAN. */
		setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	}
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
		fprintf(stderr, "netkexec_send: bad address %s\n", ip);
		return 1;
	}

	pkt = malloc(20 + DATA_MAX);
	if (payload)
		printf("netkexec: %s:%u, %ld bytes, command pack for /ramfs\n",
		       ip, port, size);
	else
		printf("netkexec: %s:%u, kernel %ld + rootfs %ld bytes, "
		       "entry 0x%08x\n", ip, port, ksize, psize, entry);

	/* Header first: the board answers with the first expected offset. */
	wr32(pkt, payload ? MAGIC_PAY : MAGIC_HDR);
	wr32(pkt + 4, entry);
	if (payload) {
		wr32(pkt + 8, (uint32_t)size);
		wr32(pkt + 12, (uint32_t)sum);
	} else {
		wr32(pkt + 8, (uint32_t)ksize);
		wr32(pkt + 12, (uint32_t)psize);
		wr32(pkt + 16, (uint32_t)sum);
	}
	for (i = 0; i < 20; i++) {
		uint32_t status;
		long ack;

		if (send_packet(fd, &dst, pkt, payload ? 16 : 20) != 0) {
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

	/* Stop-and-wait data transfer (kernel bytes, then rootfs bytes). */
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
		printf("\rnetkexec: 100%% (%ld bytes) - kernel+rootfs accepted, board is jumping\n",
		       size);
	close(fd);
	free(pkt);
	free(data);
	if (pdata)
		free(pdata);
	free(kdata);
	return 0;
}
