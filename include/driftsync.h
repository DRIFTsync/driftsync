#ifndef DRIFTSYNC_H
#define DRIFTSYNC_H

#include <inttypes.h>

#define DRIFTSYNC_PORT			4318
#define DRIFTSYNC_MAGIC			0x74667264 // 'drft'

#define DRIFTSYNC_FLAG_REPLY	(1 << 0)


// A single fixed size packet is used here for all operations to avoid an
// asymetric size between the request and reply, as that may influence the
// packet delay.

struct driftsync_packet {
	uint32_t	magic;
	uint32_t	flags;

	uint64_t	local;
		// local time on request, filled in request, preserved in reply

	uint64_t	remote;
		// remote time on reply, ignored in request, filled in reply

	uint64_t	reserved;
} __attribute__((__packed__));

#endif // DRIFTSYNC_H
