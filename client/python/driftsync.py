#!/usr/bin/env python3

import dataclasses
import select
import socket
import struct
import threading
import time


SCALE_US = 1
SCALE_MS = SCALE_US / 1000
SCALE_S = SCALE_MS / 1000


@dataclasses.dataclass
class Sample:
	local: int
	remote: int


class DRIFTsync(object):
	_DRIFTSYNC_MAGIC = 0x74667264 # 'drft'
	_DRIFTSYNC_FLAG_REPLY = (1 << 0)
	_DRIFTSYNC_PACKET_FORMAT = '<LLQQQ'
	_DRIFTSYNC_PACKET_LENGTH = struct.calcsize(_DRIFTSYNC_PACKET_FORMAT)

	def __init__(self, server, port=4318, scale=SCALE_US, interval=5,
			measureAccuracy=False):
		self._maxSamples = 10
		self._roundTripTimes = []
		self._samples = []
		self._clockRate = 1
		self._offsets = []
		self._offset = 0
		self._sentRequests = 0
		self._receivedSamples = 0
		self._rejectedSamples = 0
		self._accuracy = []
		self._quitting = threading.Event()

		self._address = socket.getaddrinfo(server, port, family=socket.AF_INET,
			proto=socket.IPPROTO_UDP)[0][-1]
		self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
		self._interrupt = socket.socketpair()

		self._lock = threading.Condition()
		self._scale = scale
		self._interval = interval
		self._measureAccuracy = measureAccuracy

		self._receiveThread = threading.Thread(target=self._receiveLoop)
		self._receiveThread.start()

		self._requestThread = threading.Thread(target=self._requestLoop)
		self._requestThread.start()

	def quit(self):
		with self._lock:
			if self._quitting.is_set():
				return

			self._quitting.set()
			self._lock.notify()

		self._socket.close()
		self._interrupt[0].send(b'0')

		self._requestThread.join()
		self._receiveThread.join()

		self._interrupt[0].close()
		self._interrupt[1].close()

	@property
	def scale(self):
		return self._scale

	@scale.setter
	def scale(self, newScale):
		self._scale = newScale

	@property
	def measureAccuracy(self):
		return self._measureAccuracy

	@measureAccuracy.setter
	def measureAccuracy(self, measureAccuracy):
		self._measureAccuracy = measureAccuracy

	def localTime(self):
		return self._localTime() * self._scale

	def globalTime(self):
		return self._globalTime() * self._scale

	@property
	def offset(self):
		return self._offset * self._scale

	@property
	def clockRate(self):
		return self._clockRate

	def suggestPlaybackRate(self, globalStartTime, playbackPosition):
		globalPosition = self._globalTime() - globalStartTime / self._scale
		difference = globalPosition - playbackPosition / self._scale
		if abs(difference) < 5000:
			return 1

		return min(2, max(0.5, 1 + difference / 1000 / 1000))

	def medianRoundTripTime(self):
		return self._medianRoundTripTime() * self._scale

	@property
	def statistics(self):
		return self._sentRequests, self._receivedSamples, self._rejectedSamples

	def accuracy(self, wait=False, reset=False, timeout=15):
		with self._lock:
			if not self._measureAccuracy:
				return 0, 0, 0

			if reset:
				self._accuracy = []

			if wait:
				try:
					self._lock.wait(timeout)
				except TimeoutError:
					return 0, 0, 0

			if not self._accuracy:
				return 0, 0, 0

			return min(self._accuracy) * self._scale, \
				sum(self._accuracy) / len(self._accuracy) * self._scale, \
				max(self._accuracy) * self._scale

	@classmethod
	def _localTime(cls):
		return time.perf_counter() * 1000 * 1000

	def _globalTime(self):
		with self._lock:
			if not self._samples:
				return 0

			reference = self._samples[-1].local
			return reference + self._offset \
				+ (self._localTime() - reference) * self._clockRate

	def _medianRoundTripTime(self):
		with self._lock:
			if not self._roundTripTimes:
				return 0
			return sorted(self._roundTripTimes)[len(self._roundTripTimes) // 2]

	def _push(self, data, value):
		if len(data) >= self._maxSamples:
			data.pop(0)
		data.append(value)

	def _sendRequest(self):
		self._sentRequests += 1
		data = struct.pack(self._DRIFTSYNC_PACKET_FORMAT, self._DRIFTSYNC_MAGIC,
			0, int(self._localTime()), 0, 0)
		self._socket.sendto(data, self._address)

	def _requestLoop(self):
		while not self._quitting.is_set():
			self._sendRequest()
			self._quitting.wait(self._interval)

	def _receiveLoop(self):
		selectArgs = ([self._socket, self._interrupt[1]], [], [])
		while not self._quitting.is_set():
			readable, _, _ = select.select(*selectArgs)
			now = self._localTime()

			if self._socket not in readable or self._quitting.is_set():
				continue

			data = self._socket.recv(self._DRIFTSYNC_PACKET_LENGTH)

			if len(data) != self._DRIFTSYNC_PACKET_LENGTH:
				continue

			magic, flags, local, remote, _ \
				= struct.unpack(self._DRIFTSYNC_PACKET_FORMAT, data)
			if magic != self._DRIFTSYNC_MAGIC:
				continue

			if (flags & self._DRIFTSYNC_FLAG_REPLY) == 0:
				continue

			if self._measureAccuracy:
				localTime = self._localTime()
				globalTime = self._globalTime()

			with self._lock:
				self._receivedSamples += 1

				roundTripTime = now - local
				self._push(self._roundTripTimes, roundTripTime)
				if abs(roundTripTime - self._medianRoundTripTime()) > 10000:
					self._rejectedSamples += 1
					continue

				self._push(self._samples, Sample(local, remote))
				if len(self._samples) >= 2:
					self._clockRate \
						= (self._samples[-1].remote - self._samples[0].remote) \
							/ (self._samples[-1].local - self._samples[0].local)

				self._push(self._offsets, remote - local)
				self._offset = sum(self._offsets) / len(self._offsets)

			if self._measureAccuracy and len(self._samples) > 1:
				globalTime -= self._globalTime()
				localTime -= self._localTime()

				with self._lock:
					self._push(self._accuracy, abs(globalTime - localTime))
					self._lock.notify()


if __name__ == '__main__':
	import sys

	sync = DRIFTsync(sys.argv[1] if len(sys.argv) > 1 else 'localhost',
		scale=SCALE_MS, measureAccuracy=True)

	if '--stream' in sys.argv:
		while True:
			print(f'{sync.globalTime():.3f}')
			time.sleep(0.005)

	remaining = 0
	while True:
		if remaining != 0:
			remaining -= 1
			if remaining == 0:
				sync.quit()
				break

		minDelta, averageDelta, maxDelta = sync.accuracy(True)
		sent, received, rejected = sync.statistics
		globalTime = sync.globalTime()

		print(f'global {globalTime:.3f} ms offset {sync.offset:.3f} ms')
		print(f'clock rate {sync.clockRate:.9f}')
		print(f'median round trip time {sync.medianRoundTripTime():.3f} ms')
		print(f'sent {sent} lost {sent - received} rejected {rejected}')
		print(f'accuracy min {minDelta:.3f} ms average {averageDelta:.3f} ms'
			f' max {maxDelta:.3f} ms')
		print()
