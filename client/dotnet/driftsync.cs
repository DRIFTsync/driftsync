using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading;


public class DRIFTsync {
	private static UInt32 DRIFTSYNC_MAGIC = 0x74667264; // 'drft'
	private static UInt32 DRIFTSYNC_FLAG_REPLY = (1 << 0);
	private static int DRIFTSYNC_PACKET_LENGTH = 32;

	private struct Sample {
		public long local;
		public long remote;
	};

	private int maxSamples = 10;
	private List<long> roundTripTimes = new List<long>();
	private List<Sample> samples = new List<Sample>();
	private double currentClockRate = 1;
	private List<long> offsets = new List<long>();
	private long averageOffset = 0;
	private int sentRequests = 0;
	private int receivedSamples = 0;
	private int rejectedSamples = 0;
	private List<long> accuracySamples = new List<long>();
	private int interval = 0;
	private UdpClient client = new UdpClient();
	private Accuracy emptyAccuracy
		= new Accuracy() { min = 0, average = 0, max = 0 };

	public struct Statistics {
		public int sentRequests;
		public int receivedSamples;
		public int rejectedSamples;
	};

	public struct Accuracy {
		public double min;
		public double average;
		public double max;
	};

	public const double SCALE_US = 1;
	public const double SCALE_MS = SCALE_US / 1000;
	public const double SALE_S = SCALE_MS / 1000;

	public double scale = 0;
	public bool measureAccuracy = false;


	public DRIFTsync(string server, int port = 4318, double scale = SCALE_US,
		int interval = 5000, bool measureAccuracy = false)
	{
		this.interval = interval;
		this.scale = scale;
		this.measureAccuracy = measureAccuracy;

		client.Connect(server, port);

		new Thread(new ThreadStart(RequestLoop)).Start();
		new Thread(new ThreadStart(ReceiveLoop)).Start();
	}

	public double localTime() {
		return _localTime() * scale;
	}

	public double globalTime() {
		return _globalTime() * scale;
	}

	public double offset {
		get { return averageOffset * scale; }
	}

	public double clockRate {
		get { return currentClockRate; }
	}

	public double suggestPlaybackRate(double globalStartTime,
		double playbackPosition) {

		double globalPosition = _globalTime() - globalStartTime / scale;
		double difference = globalPosition - playbackPosition / scale;
		if (Math.Abs(difference) < 5000)
			return 1;

		return Math.Min(2, Math.Max(0.5, 1 + difference / 1000 / 1000));
	}

	public double medianRoundTripTime() {
		return _medianRoundTripTime() * scale;
	}

	public Statistics statistics {
		get {
			return new Statistics() {
				sentRequests = sentRequests,
				receivedSamples = receivedSamples,
				rejectedSamples = rejectedSamples
			};
		}
	}

	public Accuracy accuracy(bool wait = false, bool reset = false,
		int timeout = 15000) {

		lock(this) {
			if (!measureAccuracy)
				return emptyAccuracy;

			if (reset)
				accuracySamples.Clear();

			if (wait && !Monitor.Wait(this, timeout, false))
				return emptyAccuracy;

			if (accuracySamples.Count == 0)
				return emptyAccuracy;

			return new Accuracy() {
				min = accuracySamples.Min() * scale,
				average = (double)accuracySamples.Sum()
					/ accuracySamples.Count() * scale,
				max = accuracySamples.Max() * scale
			};
		}
	}

	private static long _localTime() {
		return DateTime.Now.Ticks / 10;
	}

	private long _globalTime() {
		lock(this) {
			if (samples.Count == 0)
				return 0;

			long reference = samples[samples.Count - 1].local;
			return reference + averageOffset
				+ (long)((double)(_localTime() - reference) * currentClockRate);
		}
	}

	private long _medianRoundTripTime() {
		lock(this) {
			List<long> sorted = new List<long>(roundTripTimes);
			sorted.Sort();
			return sorted[sorted.Count / 2];
		}
	}

	private void push<T>(List<T> data, T value) {
		if (data.Count >= maxSamples)
			data.RemoveAt(0);
		data.Add(value);
	}

	private void RequestLoop() {
		byte[] buffer = new byte[DRIFTSYNC_PACKET_LENGTH];
		MemoryStream stream = new MemoryStream(buffer);
		BinaryWriter writer = new BinaryWriter(stream);

		writer.Write(DRIFTSYNC_MAGIC);
		writer.Write((UInt32)0);

		while (true) {
			sentRequests++;

			stream.Position = 8;
			writer.Write(_localTime());

			client.Send(buffer, buffer.Length);
			Thread.Sleep(interval);
		}
	}

	private void ReceiveLoop() {
		Socket socket = client.Client;
		IPEndPoint ipEndpoint = new IPEndPoint(IPAddress.Any, 0);
		EndPoint endpoint = (EndPoint)ipEndpoint;

		byte[] buffer = new byte[DRIFTSYNC_PACKET_LENGTH];
		MemoryStream stream = new MemoryStream(buffer);
		BinaryReader reader = new BinaryReader(stream);

		while (true) {
			int received = socket.ReceiveFrom(buffer, ref endpoint);
			long now = _localTime();

			if (received != DRIFTSYNC_PACKET_LENGTH)
				continue;

			stream.Position = 0;
			if (reader.ReadUInt32() != DRIFTSYNC_MAGIC)
				continue;

			if ((reader.ReadUInt32() & DRIFTSYNC_FLAG_REPLY) == 0)
				continue;

			long local = reader.ReadInt64();
			long remote = reader.ReadInt64();

			long localTime = 0;
			long globalTime = 0;
			if (measureAccuracy) {
				localTime = _localTime();
				globalTime = _globalTime();
			}

			lock (this) {
				receivedSamples++;

				long roundTripTime = now - local;
				push(roundTripTimes, roundTripTime);
				if (Math.Abs(roundTripTime - _medianRoundTripTime()) > 10000) {
					rejectedSamples++;
					continue;
				}

				push(samples, new Sample() { local = local, remote = remote });
				if (samples.Count >= 2) {
					Sample last = samples[samples.Count - 1];
					currentClockRate = (double)(last.remote - samples[0].remote)
							/ (last.local - samples[0].local);
				}

				push(offsets, remote - local);
				averageOffset = offsets.Sum() / offsets.Count;
			}

			if (measureAccuracy && samples.Count > 1) {
				globalTime -= _globalTime();
				localTime -= _localTime();

				lock(this) {
					push(accuracySamples, Math.Abs(globalTime - localTime));
					Monitor.PulseAll(this);
				}
			}
		}
	}

	public static void Main(string[] args) {
		DRIFTsync sync = new DRIFTsync(args.Length > 0 ? args[0] : "localhost",
			scale: SCALE_MS, measureAccuracy: true);

		if (args.Contains("--stream")) {
			while (true) {
				Console.WriteLine($"{sync.globalTime():f3}");
				Thread.Sleep(5);
			}
		}

		while (true) {
			Accuracy accuracy = sync.accuracy(true, timeout: 15000);
			Statistics stats = sync.statistics;
			double globalTime = sync.globalTime();
			double playbackRate = sync.suggestPlaybackRate(globalTime, 0);

			Console.WriteLine(
				$"global {globalTime:f3} ms offset {sync.offset:f3} ms");
			Console.WriteLine($"clock rate {sync.clockRate:f9}"
				+ $" {playbackRate:f9}");
			Console.WriteLine(
				$"median round trip time {sync.medianRoundTripTime():f3} ms");
			Console.WriteLine($"sent {stats.sentRequests}"
				+ $" lost {stats.sentRequests - stats.receivedSamples}"
				+ $" rejected {stats.rejectedSamples}");
			Console.WriteLine($"accuracy min {accuracy.min:f3} ms"
				+ $" average {accuracy.average:f3} ms"
				+ $" max {accuracy.max:f3} ms");
			Console.WriteLine();
		}
	}
}
