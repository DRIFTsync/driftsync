'use strict';

const dgram = require('dgram');
const dns = require('dns');


class DRIFTsync {
	static SCALE_US = 1;
	static SCALE_MS = 1 / 1000;
	static SCALE_S = 1 / 1000 / 1000;

	static DRIFTSYNC_PACKET_LENGTH = 32;
	static DRIFTSYNC_MAGIC = 0x74667264; // 'drft'
	static DRIFTSYNC_FLAG_REPLY = (1 << 0);

	constructor(server, options)
	{
		this._maxSamples = 10;
		this._roundTripTimes = [];
		this._samples = [];
		this._clockRate = 1;
		this._offsets = [];
		this._offset = 0;
		this._sentRequests = 0;
		this._receivedSamples = 0;
		this._rejectedSamples = 0;
		this._accuracy = [];
		this._accuracyWaiters = [];

		if (typeof(server) === 'object') {
			options = server;
			server = options.server;
		}

		if (!options)
			options = {};

		dns.lookup(server, { family: 4 }, (error, address, family) => {
			if (error) {
				console.error(error);
				return;
			}

			this._address = address;
		});

		this._socket = dgram.createSocket('udp4');
		this._socket.bind();
		this._socket.on('message', (buffer) => this._received(buffer));
		this._buffer = Buffer.alloc(DRIFTsync.DRIFTSYNC_PACKET_LENGTH);

		this._port = options.port || 4318;
		this._scale = options.scale || DRIFTsync.SCALE_US;
		this._interval = options.interval || 5000;
		this._measureAccuracy = !!options.measureAccuracy;

		setInterval(() => this._sendRequest(), this._interval);
	}

	get scale()
	{
		return this._scale;
	}

	set scale(newScale)
	{
		this._scale = newScale;
	}

	get measureAccuracy()
	{
		return this._measureAccuracy;
	}

	set measureAccuracy(measureAccuracy)
	{
		this._measureAccuracy = measureAccuracy;
	}

	localTime()
	{
		return this._localTime() * this._scale;
	}

	globalTime()
	{
		return this._globalTime() * this._scale;
	}

	get offset()
	{
		return this._offset * this._scale;
	}

	get clockRate()
	{
		return this._clockRate;
	}

	suggestPlaybackRate(globalStartTime, playbackPosition)
	{
		let globalPosition = this._globalTime() - globalStartTime / this._scale;
		let difference = globalPosition - playbackPosition / this._scale;
		if (Math.abs(difference) < 5000)
			return 1;

		return Math.min(2, Math.max(0.5, 1 + difference / 1000 / 1000));
	}

	medianRoundTripTime()
	{
		return this._medianRoundTripTime() * this._scale;
	}

	get statistics()
	{
		return {
			sentRequests: this._sentRequests,
			receivedSamples: this._receivedSamples,
			rejectedSamples: this._rejectedSamples
		};
	}

	accuracy(wait = false, reset = false, timeout = 15000)
	{
		const empty = { min: 0, average: 0, max: 0 };
		if (!this._measureAccuracy)
			return Promise.resolve(empty);

		if (reset)
			this._accuracy = [];

		return new Promise((resolve, reject) => {
			if (!wait) {
				resolve();
				return;
			}

			this._accuracyWaiters.push(resolve);
			if (timeout)
				setTimeout(reject, timeout);
		}).then(() => {
			if (this._accuracy.length == 0)
				return empty;

			return {
				min: Math.min(...this._accuracy) * this._scale,
				average: this._arrayAverage(this._accuracy) * this._scale,
				max: Math.max(...this._accuracy) * this._scale
			};
		}).catch(() => empty);
	}

	_localTime()
	{
		return Number(process.hrtime.bigint() / 1000n);
	}

	_globalTime()
	{
		if (this._samples.length == 0)
			return 0;

		let reference = this._samples[this._samples.length - 1].local;
		return reference + this._offset
			+ (this._localTime() - reference) * this._clockRate;
	}

	_medianRoundTripTime()
	{
		if (this._roundTripTimes.length == 0)
			return 0;

		return this._roundTripTimes.concat().sort()[
				Math.floor(this._roundTripTimes.length / 2)];
	}

	_push(data, value)
	{
		if (data.length >= this._maxSamples)
			data.shift();
		data.push(value);
	}

	_arrayAverage(array)
	{
		return array.reduce((result, value) => result + value, 0)
			/ array.length;
	}

	_sendRequest()
	{
		this._buffer.writeUInt32LE(DRIFTsync.DRIFTSYNC_MAGIC, 0);
		this._buffer.writeUInt32LE(0, 4);
		this._buffer.writeBigUInt64LE(process.hrtime.bigint() / 1000n, 8);
		this._socket.send(this._buffer, this._port, this._address);
		this._sentRequests++;
	}

	_received(buffer)
	{
		let now = this._localTime();

		if (buffer.length != DRIFTsync.DRIFTSYNC_PACKET_LENGTH)
			return;

		if (buffer.readUInt32LE(0) != DRIFTsync.DRIFTSYNC_MAGIC)
			return;

		if ((buffer.readUInt32LE(4) & DRIFTsync.DRIFTSYNC_FLAG_REPLY) == 0)
			return;

		let local = Number(buffer.readBigUInt64LE(8));
		let remote = Number(buffer.readBigUInt64LE(16));

		let localTime;
		let globalTime;
		if (this._measureAccuracy) {
			localTime = this._localTime();
			globalTime = this._globalTime();
		}

		this._receivedSamples++;

		let roundTripTime = now - local;
		this._push(this._roundTripTimes, roundTripTime)
		if (Math.abs(roundTripTime - this._medianRoundTripTime()) > 10000) {
			this._rejectedSamples++;
			return;
		}

		this._push(this._samples, {
			local: local,
			remote: remote
		});

		if (this._samples.length >= 2) {
			let last = this._samples[this._samples.length - 1];
			this._clockRate = (last.remote - this._samples[0].remote)
				/ (last.local - this._samples[0].local);
		}

		this._push(this._offsets, remote - local);
		this._offset = this._arrayAverage(this._offsets);

		if (this._measureAccuracy && this._samples.length > 1) {
			globalTime -= this._globalTime();
			localTime -= this._localTime();

			this._push(this._accuracy, Math.abs(globalTime - localTime));
			while (this._accuracyWaiters.length > 0)
				this._accuracyWaiters.shift()();
		}
	}
}


if (require.main === module) {
	let sync = new DRIFTsync({
		server: process.argv.length > 2 ? process.argv[2] : 'localhost',
		scale: DRIFTsync.SCALE_MS,
		measureAccuracy: true
	});

	let output = (accuracy) => {
		let stats = sync.statistics;
		let globalTime = sync.globalTime();
		let playbackRate = sync.suggestPlaybackRate(globalTime, 0);

		console.log(`global ${globalTime.toFixed(3)} ms offset `
			+ `${sync.offset.toFixed(3)} ms`);
		console.log(`clock rate ${sync.clockRate.toFixed(9)} `
			+ `${playbackRate.toFixed(9)}`);
		console.log('median round trip time '
			+ `${sync.medianRoundTripTime().toFixed(3)} ms`);
		console.log(`sent ${stats.sentRequests} lost `
			+ `${stats.sentRequests - stats.receivedSamples} rejected `
			+ `${stats.rejectedSamples}`);
		console.log(`accuracy min ${accuracy.min.toFixed(3)} ms average `
			+ `${accuracy.average.toFixed(3)} ms max `
			+ `${accuracy.max.toFixed(3)} ms`);
		console.log()
	};

	let loop = () => {
		sync.accuracy(true)
			.then(output)
			.then(() => loop());
	};

	if (process.argv.includes('--stream'))
		setInterval(() => console.log(sync.globalTime().toFixed(3)), 5);
	else
		loop();
}


module.exports = DRIFTsync;
