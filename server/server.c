#include <driftsync.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

#include <netinet/in.h>


static inline uint64_t
localTime()
{
	struct timespec time;
	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		return 0;

	return ((uint64_t)time.tv_sec * 1000 * 1000 * 1000 + time.tv_nsec) / 1000;
}


int
main(int argc, char *argv[])
{
	int verbose = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
			verbose = 1;
		else {
			printf("usage: %s [-v|--verbose]\n", argv[0]);
			exit(1);
		}
	}

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = &exit;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		printf("failed to create socket: %s\n", strerror(errno));
		return 1;
	}

	int reuse = 1;
	int result = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse,
		sizeof(reuse));
	if (result != 0) {
		printf("failed to set address reuse socket option: %s\n",
			strerror(errno));
		// non-fatal
	}

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(DRIFTSYNC_PORT);
	result = bind(sock, (struct sockaddr *)&address, sizeof(address));
	if (result != 0) {
		printf("failed to bind to local port: %s\n", strerror(errno));
		return 1;
	}

	struct sockaddr_storage remote;
	struct driftsync_packet packet;
	while (1) {
		socklen_t remoteLength = sizeof(remote);
		result = recvfrom(sock, &packet, sizeof(packet), 0,
			(struct sockaddr *)&remote, &remoteLength);

		if (result < 0) {
			printf("failed to receive: %s\n", strerror(errno));
			continue;
		}

		if (result < (int)sizeof(packet)) {
			printf("received incomplete packet of %d\n", result);
			continue;
		}

		if (packet.magic != DRIFTSYNC_MAGIC) {
			printf("protocol mismatch\n");
			continue;
		}

		if ((packet.flags & DRIFTSYNC_FLAG_REPLY) != 0) {
			printf("received reply packet\n");
			continue;
		}

		packet.flags |= DRIFTSYNC_FLAG_REPLY;
		packet.remote = localTime();
		result = sendto(sock, &packet, sizeof(packet), 0,
			(struct sockaddr *)&remote, remoteLength);

		if (verbose) {
			printf("processed request packet, remote time %" PRIu64
				", local time %" PRIu64 "\n", packet.local, packet.remote);
		}

		if (result < 0) {
			printf("failed to send: %s\n", strerror(errno));
			continue;
		}

		if (result != (int)sizeof(packet)) {
			printf("sent incomplete packet of %d\n", result);
			continue;
		}
	}

	return 0;
}
