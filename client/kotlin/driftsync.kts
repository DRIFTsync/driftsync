package org.driftsync

import java.net.InetAddress
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.locks.ReentrantLock
import java.util.concurrent.TimeUnit

import kotlin.concurrent.thread
import kotlin.concurrent.withLock
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min


val SCALE_US = 1.0
val SCALE_MS = SCALE_US / 1000
val SCALE_S = SCALE_MS / 1000

class DRIFTsync(
	val server: String,
	val port: Int = 4318,
	var scale: Double = 1.0,
	var interval: Long = 5000,
	var measureAccuracy: Boolean = false
) {

	private class Sample(val local: Long, val remote: Long)

	class Statistics(
		val sent: Int,
		val received: Int,
		val rejected: Int
	)

	class Accuracy(val min: Double, val average: Double, val max: Double)

	private val DRIFTSYNC_MAGIC = 0x74667264 // 'drft'
	private val DRIFTSYNC_FLAG_REPLY = 1
	private val DRIFTSYNC_PACKET_LENGTH = 32

	private val maxSamples = 10
	private var roundTripTimes : MutableList<Long> = mutableListOf()
	private var samples: MutableList<Sample> = mutableListOf()
	private var currentClockRate: Double = 1.0
	private var offsets: MutableList<Long> = mutableListOf()
	private var averageOffset: Long = 0
	private var sentRequests = 0
	private var receivedSamples = 0
	private var rejectedSamples = 0
	private var accuracySamples: MutableList<Long> = mutableListOf()

	private val socket = DatagramSocket()
	private val lock = ReentrantLock()
	private val condition = lock.newCondition()

	fun localTime() = _localTime() * scale
	fun globalTime() = _globalTime() * scale

	val offset get() = averageOffset * scale
	val clockRate get() = currentClockRate

	fun suggestPlaybackRate(
		globalStartTime: Double, playbackPosition: Double
	): Double {
		val globalPosition = _globalTime() - (globalStartTime / scale).toLong()
		val difference = globalPosition - playbackPosition / scale
		if (abs(difference) < 5000)
			return 1.0

		return min(2.0, max(0.5, 1.0 + difference / 1000 / 1000))
	}

	fun medianRoundTripTime() = _medianRoundTripTime() * scale

	val statistics
		get() = Statistics(sentRequests, receivedSamples, rejectedSamples)

	fun accuracy(
		wait: Boolean = false,
		reset: Boolean = false,
		timeout: Long = 15000
	): Accuracy {
		val empty = Accuracy(0.0, 0.0, 0.0)
		if (!measureAccuracy)
			return empty

		lock.withLock {
			if (reset)
				accuracySamples.clear()

			if (wait && !condition.await(timeout, TimeUnit.MILLISECONDS))
				return empty

			if (accuracySamples.isNullOrEmpty())
				return empty

			return Accuracy(accuracySamples.minOrNull()!!.toDouble() * scale,
				accuracySamples.average() * scale,
				accuracySamples.maxOrNull()!!.toDouble() * scale)
		}
	}

	private fun _localTime() = System.nanoTime() / 1000

	private fun _globalTime(): Long {
		lock.withLock {
			if (samples.size == 0)
				return 0

			val reference = samples.lastOrNull()?.local ?: 0
			return (reference + averageOffset + (_localTime() - reference)
					* currentClockRate).toLong()
		}
	}

	private fun _medianRoundTripTime(): Long {
		lock.withLock {
			return roundTripTimes.sorted()[roundTripTimes.size / 2]
		}
	}

	private fun <T: Any> _push(data: MutableList<T>, value: T) {
		if (data.size >= maxSamples)
			data.removeAt(0)
		data.add(value)
	}

	private val requestThread = thread() {
		val address = InetAddress.getByName(server)
		var buffer = ByteBuffer.allocate(DRIFTSYNC_PACKET_LENGTH)
		var packet = DatagramPacket(buffer.array(), DRIFTSYNC_PACKET_LENGTH,
			address, port)

		buffer.order(ByteOrder.LITTLE_ENDIAN)
		buffer.putInt(DRIFTSYNC_MAGIC)
		buffer.putInt(0)
		buffer.mark()

		while (true) {
			sentRequests++
			buffer.reset()
			buffer.putLong(_localTime())
			socket.send(packet)

			Thread.sleep(interval)
		}
	}

	private val receiveThread = thread() {
		var buffer = ByteBuffer.allocate(DRIFTSYNC_PACKET_LENGTH)
		var packet = DatagramPacket(buffer.array(), DRIFTSYNC_PACKET_LENGTH)

		buffer.order(ByteOrder.LITTLE_ENDIAN)

		while (true) {
			socket.receive(packet)
			val now = _localTime()

			if (packet.length != DRIFTSYNC_PACKET_LENGTH)
				continue

			buffer.rewind()
			if (buffer.getInt() != DRIFTSYNC_MAGIC)
				continue

			if ((buffer.getInt() and DRIFTSYNC_FLAG_REPLY) == 0)
				continue

			val local = buffer.getLong()
			val remote = buffer.getLong()

			var localTime: Long = 0
			var globalTime: Long = 0
			if (measureAccuracy) {
				localTime = _localTime()
				globalTime = _globalTime()
			}

			lock.withLock {
				receivedSamples++

				val roundTripTime = now - local
				_push(roundTripTimes, roundTripTime)
				if (abs(roundTripTime - _medianRoundTripTime()) > 10000) {
					rejectedSamples++
				} else {
					_push(samples, Sample(local, remote))
					if (samples.size >= 2) {
						val last = samples[samples.lastIndex]
						currentClockRate = ((last.remote - samples[0].remote)
								.toDouble() / (last.local - samples[0].local))
					}

					_push(offsets, remote - local)
					averageOffset = offsets.average().toLong()
				}
			}

			if (measureAccuracy && samples.size > 1) {
				globalTime -= _globalTime()
				localTime -= _localTime()

				lock.withLock {
					_push(accuracySamples, abs(globalTime - localTime))
					condition.signalAll()
				}
			}
		}
	}
}

var sync = DRIFTsync(args.firstOrNull() ?: "localhost", scale = SCALE_MS,
	measureAccuracy = true)

fun Double.fix(length: Int) = "%.${length}f".format(this)

if (args.contains("--stream")) {
	while (true) {
		println(sync.globalTime().fix(3))
		Thread.sleep(5)
	}
}

while (true) {
	val accuracy = sync.accuracy(true, timeout = 15000)
	val stats = sync.statistics
	val globalTime = sync.globalTime()
	val playbackRate = sync.suggestPlaybackRate(globalTime, 0.0)

	println("global ${globalTime.fix(3)} ms offset ${sync.offset.fix(3)} ms")
	println("clock rate ${sync.clockRate.fix(9)} ${playbackRate.fix(9)}")
	println("median round trip time ${sync.medianRoundTripTime().fix(3)} ms")
	println("sent ${stats.sent} lost ${stats.sent - stats.received}"
		+ " rejected ${stats.rejected}")
	println("accuracy min ${accuracy.min.fix(3)} ms average"
		+ " ${accuracy.average.fix(3)} ms max ${accuracy.max.fix(3)} ms")
	println()
}
