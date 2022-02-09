#include <driftsync.h>

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


#define SCALE_US 1.0
#define SCALE_MS SCALE_US / 1000
#define SALE_S = SCALE_MS / 1000


struct sample {
	int64_t local;
	int64_t remote;
};


struct statistics {
	int sentRequests;
	int receivedSamples;
	int rejectedSamples;
};


struct accuracy {
	double min;
	double average;
	double max;
};


struct ring_buffer {
	void *buffer;
	size_t size;
	size_t elementSize;
	size_t count;
	size_t position;
};


static void
ring_buffer_init(struct ring_buffer *buffer, size_t size, size_t elementSize)
{
	buffer->buffer = calloc(size, elementSize);
	buffer->size = size;
	buffer->elementSize = elementSize;
	buffer->count = 0;
	buffer->position = size - 1;
}


static void
ring_buffer_destroy(struct ring_buffer *buffer)
{
	free(buffer->buffer);
}


static void
ring_buffer_push(struct ring_buffer *buffer, void *data)
{
	buffer->position = (buffer->position + 1) % buffer->size;
	if (buffer->count < buffer->size)
		buffer->count++;

	memcpy((uint8_t *)buffer->buffer + buffer->position * buffer->elementSize,
		data, buffer->elementSize);
}


static void *
ring_buffer_get(struct ring_buffer *buffer, size_t position)
{
	position += buffer->position - buffer->count + 1;
	return (uint8_t *)buffer->buffer
		+ (position % buffer->size) * buffer->elementSize;
}


static void
ring_buffer_copy(struct ring_buffer *from, struct ring_buffer *to)
{
	assert(from->elementSize == to->elementSize);
	assert(from->size == to->size);

	memcpy(to->buffer, from->buffer, from->size * from->elementSize);
	to->count = from->count;
	to->position = from->position;
}


static void
ring_buffer_clear(struct ring_buffer *buffer)
{
	memset(buffer->buffer, 0, buffer->size * buffer->elementSize);
	buffer->count = 0;
	buffer->position = buffer->size - 1;
}


static void
ring_buffer_apply(struct ring_buffer *buffer,
	void (*op)(void *data, void *state), void *state)
{
	for (size_t i = 0; i < buffer->count; i++)
		op(ring_buffer_get(buffer, i), state);
}


static void *
ring_buffer_median(struct ring_buffer *buffer,
	int (*compar)(const void *, const void *))
{
	qsort(buffer->buffer, buffer->count, buffer->elementSize, compar);
	return ring_buffer_get(buffer, buffer->count / 2);
}


struct DRIFTsync {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	size_t maxSamples;
	int socket;
	struct sockaddr_storage server;
	struct ring_buffer roundTripTimes;
	struct ring_buffer sortedRoundTripTimes;
	struct ring_buffer samples;
	double clockRate;
	struct ring_buffer offsets;
	int64_t averageOffset;
	struct ring_buffer accuracySamples;
	struct statistics statistics;
	int interval;
	double scale;
	int measureAccuracy;
};


static inline int64_t
localTime()
{
	struct timespec time;
	if (clock_gettime(CLOCK_MONOTONIC, &time) == 0)
		return (int64_t)time.tv_sec * 1000 * 1000 + time.tv_nsec / 1000;
	return 0;
}


static int64_t
globalTime(struct DRIFTsync *sync)
{
	pthread_mutex_lock(&sync->lock);

	int64_t result = 0;
	if (sync->samples.count > 0) {
		int64_t reference = ((struct sample *)ring_buffer_get(&sync->samples,
				sync->samples.count - 1))->local;
		result = reference + sync->averageOffset
			+ (int64_t)((localTime() - reference) * sync->clockRate);
	}

	pthread_mutex_unlock(&sync->lock);
	return result;
}


static int
compare_int64_t(const void *one, const void *two)
{
	return *(int64_t *)one - *(int64_t *)two;
}


static int64_t
medianRoundTripTime(struct DRIFTsync *sync, int locked)
{
	if (!locked)
		pthread_mutex_lock(&sync->lock);

	ring_buffer_copy(&sync->roundTripTimes, &sync->sortedRoundTripTimes);
	int64_t result = *(int64_t *)ring_buffer_median(&sync->sortedRoundTripTimes,
		compare_int64_t);

	if (!locked)
		pthread_mutex_unlock(&sync->lock);

	return result;
}


static void *
request_loop(void *data)
{
	struct DRIFTsync *sync = (struct DRIFTsync *)data;

	struct driftsync_packet packet;
	memset(&packet, 0, sizeof(packet));
	packet.magic = DRIFTSYNC_MAGIC;

	while (1) {
		sync->statistics.sentRequests++;

		packet.local = localTime();
		int result = sendto(sync->socket, &packet, sizeof(packet), 0,
			(struct sockaddr *)&sync->server, sizeof(sync->server));

		if (result < 0) {
			printf("failed to send: %s\n", strerror(errno));
			continue;
		}

		if (result != (int)sizeof(packet)) {
			printf("sent incomplete packet of %d\n", result);
			continue;
		}

		usleep(sync->interval);
	}

	return NULL;
}


static void
sum_int64_t(void *data, void *state)
{
	*(int64_t *)state += *(int64_t *)data;
}


static void *
receive_loop(void *data)
{
	struct DRIFTsync *sync = (struct DRIFTsync *)data;

	struct sockaddr_storage peer;
	socklen_t remoteLength = sizeof(peer);
	struct driftsync_packet packet;

	while (1) {
		int result = recvfrom(sync->socket, &packet, sizeof(packet), 0,
			(struct sockaddr *)&peer, &remoteLength);
		int64_t now = localTime();

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

		if ((packet.flags & DRIFTSYNC_FLAG_REPLY) == 0) {
			printf("received request packet\n");
			continue;
		}

		int64_t measureLocalTime = 0;
		int64_t measureGlobalTime = 0;
		if (sync->measureAccuracy) {
			measureLocalTime = localTime();
			measureGlobalTime = globalTime(sync);
		}

		pthread_mutex_lock(&sync->lock);
		sync->statistics.receivedSamples++;

		int64_t roundTripTime = now - packet.local;
		ring_buffer_push(&sync->roundTripTimes, &roundTripTime);
		int64_t difference = roundTripTime - medianRoundTripTime(sync, 1);
		if ((difference < 0 ? -difference : difference) > 10000) {
			sync->statistics.rejectedSamples++;
			pthread_mutex_unlock(&sync->lock);
			continue;
		}

		struct sample sample = {
			.local = packet.local,
			.remote = packet.remote
		};

		ring_buffer_push(&sync->samples, &sample);
		if (sync->samples.count >= 2) {
			struct sample *first = (struct sample *)ring_buffer_get(
				&sync->samples, 0);
			struct sample *last = (struct sample *)ring_buffer_get(
				&sync->samples, sync->samples.count - 1);

			sync->clockRate = (double)(last->remote - first->remote)
				/ (last->local - first->local);
		}

		int64_t offset = packet.remote - packet.local;
		ring_buffer_push(&sync->offsets, &offset);

		int64_t total = 0;
		ring_buffer_apply(&sync->offsets, &sum_int64_t, &total);

		sync->averageOffset = total / sync->offsets.count;
		pthread_mutex_unlock(&sync->lock);

		if (sync->measureAccuracy && sync->samples.count > 1) {
			measureGlobalTime -= globalTime(sync);
			measureLocalTime -= localTime();

			pthread_mutex_lock(&sync->lock);

			int64_t accuracySample = measureGlobalTime - measureLocalTime;
			if (accuracySample < 0)
				accuracySample = -accuracySample;
			ring_buffer_push(&sync->accuracySamples, &accuracySample);

			pthread_cond_broadcast(&sync->condition);
			pthread_mutex_unlock(&sync->lock);
		}
	}

	return NULL;
}


void
DRIFTsync_destroy(struct DRIFTsync *sync)
{
	close(sync->socket);

	ring_buffer_destroy(&sync->roundTripTimes);
	ring_buffer_destroy(&sync->sortedRoundTripTimes);
	ring_buffer_destroy(&sync->samples);
	ring_buffer_destroy(&sync->accuracySamples);

	pthread_cond_destroy(&sync->condition);
	pthread_mutex_destroy(&sync->lock);
}


struct DRIFTsync *
DRIFTsync_create(const char *server, uint16_t port, double scale, int interval,
	int measureAccuracy)
{
	struct DRIFTsync *sync
		= (struct DRIFTsync *)malloc(sizeof(struct DRIFTsync));
	if (sync == NULL) {
		printf("out of memory allocating sync struct\n");
		return NULL;
	}

	sync->socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sync->socket < 0) {
		printf("failed to create socket: %s\n", strerror(errno));
		free(sync);
		return NULL;
	}

	char service[10];
	snprintf(service, sizeof(service), "%u", port);

	struct addrinfo *addressInfo;
	int result = getaddrinfo(server, service, NULL, &addressInfo);
	if (result != 0 || addressInfo == NULL) {
		printf("failed to resolve host \"%s\": %s\n", server,
			gai_strerror(result));
		free(sync);
		return NULL;
	}

	memcpy(&sync->server, addressInfo->ai_addr, addressInfo->ai_addrlen);
	freeaddrinfo(addressInfo);

	pthread_mutex_init(&sync->lock, NULL);
	pthread_cond_init(&sync->condition, NULL);

	sync->maxSamples = 10;
	sync->clockRate = 1.0;
	sync->averageOffset = 0;
	memset(&sync->statistics, 0, sizeof(struct statistics));

	ring_buffer_init(&sync->roundTripTimes, sync->maxSamples, sizeof(int64_t));
	ring_buffer_init(&sync->sortedRoundTripTimes, sync->maxSamples,
		sizeof(int64_t));
	ring_buffer_init(&sync->samples, sync->maxSamples, sizeof(struct sample));
	ring_buffer_init(&sync->offsets, sync->maxSamples, sizeof(int64_t));
	ring_buffer_init(&sync->accuracySamples, sync->maxSamples, sizeof(int64_t));

	sync->interval = interval;
	sync->scale = scale;
	sync->measureAccuracy = measureAccuracy;

	pthread_t thread;
	pthread_create(&thread, NULL, &receive_loop, sync);
	pthread_create(&thread, NULL, &request_loop, sync);

	return sync;
}


double
DRIFTsync_localTime(struct DRIFTsync *sync)
{
	return localTime() * sync->scale;
}


double
DRIFTsync_globalTime(struct DRIFTsync *sync)
{
	return globalTime(sync) * sync->scale;
}


double
DRIFTsync_offset(struct DRIFTsync *sync)
{
	return sync->averageOffset * sync->scale;
}


double
DRIFTsync_clockRate(struct DRIFTsync *sync)
{
	return sync->clockRate;
}


double
DRIFTsync_suggestPlaybackRate(struct DRIFTsync *sync, double globalStartTime,
	double playbackPosition)
{
	double globalPosition = globalTime(sync) - globalStartTime / sync->scale;
	double difference = globalPosition - playbackPosition / sync->scale;
	if ((difference < 0 ? -difference : difference) < 5000)
		return 1;

	double rate = 1 + difference / 1000 / 1000;
	return rate > 2 ? 2 : rate < 0.5 ? 0.5 : rate;
}


double
DRIFTsync_medianRoundTripTime(struct DRIFTsync *sync)
{
	return medianRoundTripTime(sync, 0) * sync->scale;
}


void
DRIFTsync_statistics(struct DRIFTsync *sync, struct statistics *stats)
{
	pthread_mutex_lock(&sync->lock);
	memcpy(stats, &sync->statistics, sizeof(struct statistics));
	pthread_mutex_unlock(&sync->lock);
}


static void
accumulate_accuracy(void *_data, void *_state)
{
	int64_t *data = (int64_t *)_data;
	struct accuracy *state = (struct accuracy *)_state;

	if (*data < state->min)
		state->min = *data;
	if (*data > state->max)
		state->max = *data;

	state->average += *data;
}


void
DRIFTsync_accuracy(struct DRIFTsync *sync, struct accuracy *accuracy, int wait,
	int reset, int timeout)
{
	accuracy->min = accuracy->average = accuracy->max = 0.0;

	if (!sync->measureAccuracy)
		return;

	pthread_mutex_lock(&sync->lock);

	if (reset)
		ring_buffer_clear(&sync->accuracySamples);

	if (wait) {
		if (timeout > 0) {
			struct timespec spec;
			clock_gettime(CLOCK_REALTIME, &spec);
			spec.tv_sec += timeout / 1000 / 1000;
			spec.tv_nsec += (timeout % 1000 / 1000) * 1000;

			if (pthread_cond_timedwait(&sync->condition, &sync->lock, &spec)
					!= 0) {
				pthread_mutex_unlock(&sync->lock);
				return;
			}
		} else {
			if (pthread_cond_wait(&sync->condition, &sync->lock) != 0) {
				pthread_mutex_unlock(&sync->lock);
				return;
			}
		}
	}

	if (sync->accuracySamples.count == 0) {
		pthread_mutex_unlock(&sync->lock);
		return;
	}

	accuracy->min = DBL_MAX;

	ring_buffer_apply(&sync->accuracySamples, &accumulate_accuracy, accuracy);

	accuracy->average /= sync->accuracySamples.count;

	accuracy->min *= sync->scale;
	accuracy->average *= sync->scale;
	accuracy->max *= sync->scale;

	pthread_mutex_unlock(&sync->lock);
}


int
main(int argc, char *argv[])
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = &exit;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	struct DRIFTsync *sync = DRIFTsync_create(argc > 1 ? argv[1] : "localhost",
		DRIFTSYNC_PORT, SCALE_MS, 5000 * 1000, 1);
	if (sync == NULL)
		return 1;

	int stream = 0;
	for (int i = 1; i < argc && !stream; i++)
		stream = strcmp(argv[i], "--stream") == 0;

	if (stream) {
		while (1) {
			printf("%.3f\n", DRIFTsync_globalTime(sync));
			usleep(5 * 1000);
		}
	}

	while (1) {
		struct accuracy accuracy;
		DRIFTsync_accuracy(sync, &accuracy, 1, 0, 15000 * 1000);

		struct statistics stats;
		DRIFTsync_statistics(sync, &stats);

		double globalTime = DRIFTsync_globalTime(sync);

		printf("global %.3f ms offset %.3f ms\n", globalTime,
			DRIFTsync_offset(sync));
		printf("clock rate %.9f %.9f\n", DRIFTsync_clockRate(sync),
			DRIFTsync_suggestPlaybackRate(sync, globalTime, 0));
		printf("median round trip time %.3f ms\n",
			DRIFTsync_medianRoundTripTime(sync));
		printf("sent %d lost %d rejected %d\n",
			stats.sentRequests, stats.sentRequests - stats.receivedSamples,
			stats.rejectedSamples);
		printf("accuracy min %.3f ms average %.3f ms max %.3f ms\n\n",
			accuracy.min, accuracy.average, accuracy.max);
		fflush(stdout);
	}

	return 0;
}
