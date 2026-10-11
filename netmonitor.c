/* Show the board's netlog stream (UDP, default port 19290) on the host: the
 * network equivalent of `make monitor`.
 *
 *   netmonitor [port]
 *
 * Prints one line per datagram with a timestamp, so it plays well with
 * `make netmon | tee board.log`.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEF_PORT 19290

int main(int argc, char **argv)
{
	unsigned int port = DEF_PORT;
	struct sockaddr_in addr;
	unsigned char buf[4096];
	int fd, on = 1;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [port]\n", argv[0]);
		return 2;
	}
	if (argc == 2)
		port = (unsigned int)strtoul(argv[1], NULL, 0);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}

	fprintf(stderr, "netmonitor: listening on UDP %u (Ctrl-C to exit)\n", port);
	for (;;) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
				     (struct sockaddr *)&from, &fromlen);
		time_t now;
		struct tm tm;
		char stamp[16];
		const char *ip;
		unsigned int i;

		if (n < 0)
			continue;
		buf[n] = '\0';
		now = time(NULL);
		localtime_r(&now, &tm);
		strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
		ip = inet_ntoa(from.sin_addr);

		/* Prefix the first line; keep continuation lines readable. */
		printf("[%s %s] ", stamp, ip);
		for (i = 0; i < (unsigned int)n; i++) {
			putchar(buf[i]);
			if (buf[i] == '\n' && i + 1 < (unsigned int)n)
				printf("[%s %s] ", stamp, ip);
		}
		if (n == 0 || buf[n - 1] != '\n')
			putchar('\n');
		fflush(stdout);
	}
}
