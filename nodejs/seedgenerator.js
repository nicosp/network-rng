var util = require('util');
var net = require('net');
var buffer = require('buffer');
var events = require('events');

var REMOTE_CONNECT_RETRY_TIME = 1000;
var SEED_BUFFER_DEFAULT_SIZE = 1024*1024;

function default_param(param, def) {
	return typeof param !== 'undefined' ? param : def;
}

function SeedBuffer(capacity) {
	this.buffer = new Buffer(capacity);
	this.size = 0;
	this.beginIndex = 0;
	this.endIndex = 0;
}

SeedBuffer.prototype.write = function (buf) {
	var capacity;
	var bytesToWrite;

	if (buf.length <= 0) return 0;

	capacity = this.buffer.length;
	bytesToWrite = (buf.length > this.getSpace())? this.getSpace(): buf.length;

 	/* We can write everything in one go */
	if (bytesToWrite <= capacity - this.endIndex) {
		buf.copy(this.buffer, this.endIndex, 0, bytesToWrite);

		this.endIndex += bytesToWrite;
		if (this.endIndex === capacity) this.endIndex= 0;
  	} else {
		var size_1 = capacity - this.endIndex;

		buf.copy(this.buffer, this.endIndex, 0, size_1);
		buf.copy(this.buffer, 0, size_1, bytesToWrite);

		this.endIndex = bytesToWrite - size_1;
	}

	this.size += bytesToWrite;

	return bytesToWrite;
};

SeedBuffer.prototype.available = function() {
	return this.size;
};

SeedBuffer.prototype.capacity = function() {
	return this.buffer.length;
};

SeedBuffer.prototype.isFull = function() {
	return this.capacity() === this.available();
};

SeedBuffer.prototype.getSpace = function() {
	return this.capacity() - this.available();
};

SeedBuffer.prototype.read = function(buf)
{
	var capacity;
	var bytesToRead;

	if (buf.length <= 0) return 0;

	capacity = this.capacity();
	bytesToRead = (buf.length > this.available())?this.available():buf.length;

	/* Read in a single step */
	if (bytesToRead <= capacity - this.beginIndex) {
		this.buffer.copy(buf, 0, this.beginIndex, this.beginIndex + bytesToRead);
		this.beginIndex += bytesToRead;
  	} else {
		var size_1 = capacity - this.beginIndex;
		var size_2 = bytesToRead - size_1;

		this.buffer.copy(buf, 0, this.beginIndex, this.beginIndex + size_1);
		this.buffer.copy(buf, size_1, 0, size_2);
		this.beginIndex = size_2;
  	}
	if (this.beginIndex === capacity) this.beginIndex = 0;

  	this.size -= bytesToRead;

	return bytesToRead;
};


/**
connectOptions - See: net.connect
*/
function SeedGenerator(connectOptions, size, autoconnect, connectRetryTime) {
	if (typeof connectOptions === 'undefined') {
		throw new Error('connectOptions are required');
	}

	this.connectOptions = connectOptions;

	this.autoconnect = default_param(autoconnect, true);
	this.connected = false;

	this.seedBuf = new SeedBuffer(default_param(size, SEED_BUFFER_DEFAULT_SIZE));

	this.seedPending = 0;
	this.bytesPending = 0;
	this.headerBuf = new Buffer(4);
	this.headerBytesPending = 0;

	this.idleInterval = null;
	this.connectTimeout = null;
	this.connectRetryTime = default_param(connectRetryTime, REMOTE_CONNECT_RETRY_TIME);
	this.reconnectCount = 0;
	this.readyTriggered = false;
	this.seedWaiters = new Array();
};

util.inherits(SeedGenerator, events.EventEmitter);

SeedGenerator.prototype.connect = function() {
	var self = this;

	if (this.connected === true) {
		console.log('Already connected!');
		return;
	}

	if (self.idleInterval) {
		clearInterval(self.idleInterval);
		self.idleInterval = null;
	}

	if (self.connectTimeout) {
		clearTimeout(self.connectTimeout);
		self.connectTimeout = null;
	}

	this.client = net.connect(self.connectOptions,
		function() {
			self.connected = true;
			self.reconnectCount = 0;

			/* Idle handling. The server will close all idle connections after 30 seconds.
			*/
			self.idleInterval = setInterval(
						function(generator) {
							generator.sendIdle();
						}, 10000, self);

			self.emit('connect');
			self.fill();
		}
	);

	this.client.on('data', 
		function(chunk) {
			var endOffset = chunk.length;
			var bytesConsumed = 0;
			var seedSize = 0;
                        var chunkBuf;

			//console.log('Received packet. Size: ' + chunk.length);

			while(bytesConsumed < chunk.length) {
				/* Handle partial headers */
				if (self.headerBytesPending > 0) {
					chunk.copy(self.headerBuf, 4 - self.headerBytesPending, bytesConsumed, 4);
					self.headerBytesPending = 0;
					self.bytesPending = self.headerBuf.readUInt32BE(0);
					self.seedPending -= self.bytesPending;
					bytesConsumed += 4;

				} else if (self.bytesPending === 0) {
					/* Complete header */
					if ((chunk.length - bytesConsumed) >= 4) {
						self.bytesPending = chunk.readUInt32BE(bytesConsumed);
						self.seedPending -= self.bytesPending;
						bytesConsumed += 4;
					} else {
						chunk.copy(self.headerBuf, 0, bytesConsumed);
						self.bytesPending = 0;
						self.headerBytesPending = 4 - chunk.length;
						break;
					}
				}

				if (chunk.length >= (self.bytesPending + bytesConsumed)) {
					endOffset = bytesConsumed + self.bytesPending;
				} else {
					endOffset = chunk.length;
				}

				seedSize = endOffset - bytesConsumed;
				self.bytesPending -= seedSize;

				/* Write in buf */
                                chunkBuf = chunk.slice(bytesConsumed, endOffset);

				self.seedBuf.write(chunkBuf);

				bytesConsumed = endOffset;

				self.emit('entropy-received', chunkBuf);
			}

			/* Send available data to seed waiters */
			while(self.seedWaiters.length > 0) {
				var seedWaiter = self.seedWaiters[0];

				if (seedWaiter.buffer.length > self.getSeedAvailable()) {
					break;
				}

				self.seedWaiters.shift();

				var seedBuf = seedWaiter.buffer;
				var seedCallback = seedWaiter.callback;

				self.seedBuf.read(seedBuf);

				if (seedWaiter.timer !== null) {
					clearTimeout(seedWaiter.timer);
				}

				global.setImmediate(function() { seedCallback(seedBuf); });
			}

			if (self.readyTriggered === false && self.seedBuf.isFull()) {
				self.readyTriggered = true;
				self.emit('ready');
			}

			self.fill();
		}
	);

	this.client.on('end',
		function() {
			if (self.idleInterval) {
				clearInterval(self.idleInterval);
				self.idleInterval = null;
			}

			self.headerBytesPending = 0;
			self.bytesPending = 0;
			self.seedPending = 0;
			self.connected = false;

			self.emit('disconnect');

			if (self.autoconnect === true) {
				self.reconnect();
			} else {
				while(self.seedWaiters.length > 0) {
					var seedWaiter = self.seedWaiters.shift();

					var seedCallback = seedWaiter.errorCallback;

					if (seedWaiter.timer !== null) {
						clearTimeout(seedWaiter.timer);
					}

					global.setImmediate(function() { seedCallback(); });
				}
			}
		}
	);

	this.client.on('error',
		function(msg) {
			if (self.idleInterval) {
				clearInterval(self.idleInterval);
				self.idleInterval = null;
			}

			self.headerBytesPending = 0;
			self.bytesPending = 0;
			self.seedPending = 0;
			self.connected = false;

			self.emit('disconnect', msg);

			if (self.autoconnect === true) {
				self.reconnect();
			} else {
				while(self.seedWaiters.length > 0) {
					var seedWaiter = self.seedWaiters.shift();

					var seedCallback = seedWaiter.errorCallback;

					if (seedWaiter.timer !== null) {
						clearTimeout(seedWaiter.timer);
					}

					global.setImmediate(function() { seedCallback(); });
				}
			}
		}
	);
};

SeedGenerator.prototype.sendIdle = function() {
	var buf = new Buffer(4);
	buf.writeUInt32BE(0, 0);
	this.client.write(buf);
};

SeedGenerator.prototype.fill = function() {
	var entropyMissing = this.seedBuf.getSpace() - (this.seedPending + this.bytesPending);

	/* Don't ask the remote source for little amounts of entropy but wait until more is needed */
	if (entropyMissing < 4096 && entropyMissing < this.seedBuf.capacity()) {
		return;
	}

	if (this.connected === true) {
		//console.log('Requesting ' + entropyMissing + ' bytes of entropy from server');
		var buf = new Buffer(4);
		buf.writeUInt32BE(entropyMissing, 0);
		this.client.write(buf);
		this.seedPending += entropyMissing;
	}
};

SeedGenerator.prototype.reconnect = function() {
	var delay;
	var self = this;

	if (self.connected === true) {
		throw new Error('Already connected!');
	}

	if (self.connectTimeout === null) {
		delay = self.connectRetryTime + (self.connectRetryTime * self.reconnectCount * 1.5);

		if (delay < self.connectRetryTime * 10) {
			self.reconnectCount++;
		}

		console.log('Attempting connection after ' + delay + 'ms');

		self.connectTimeout = setTimeout(function() { self.connect(); }, delay);
	}
};

SeedGenerator.prototype.stop = function() {
	if (this.connectTimeout) {
		clearTimeout(this.connectTimeout);
		this.connectTimeout = null;
	}

	if (this.idleInterval) {
		clearInterval(this.idleInterval);
		this.idleInterval = null;
	}

	this.autoconnect = false;
	this.connected = false;
	this.client.end();
};

/*
 Fills a buffer with random bytes suitable for seeding an RNG.
 This function will never block and it will fail immediately if
 there are not enough random bytes.
*/
SeedGenerator.prototype.getSeed = function(buffer) {
	if (this.seedBuf.available() < buffer.length) {
		throw new Error('No entropy available');
	}

	this.seedBuf.read(buffer);
	this.fill();
};

SeedGenerator.prototype.getSeedAsync = function(buffer, callback, errorCallback, timeout) {
	var self = this;

	if (self.seedBuf.available() < buffer.length) {
		if (self.connected === false && self.autoconnect === false) {
			global.setImmediate(errorCallback);
			return;
		}

		var timeoutTimer = null;
		if (typeof errorCallback !== 'undefined') {
			timeoutTimer = setTimeout(errorCallback, timeout);
		}

		if (typeof timeout !== 'undefined') {
			timeout = 50;
		}

		var req = { buffer: buffer, callback: callback, errorCallback: errorCallback, timer: timeoutTimer };
		this.seedWaiters.push(req);

	} else {
		self.seedBuf.read(buffer);
		self.fill();

		global.setImmediate(function() { callback(buffer); });
	}
};


SeedGenerator.prototype.getSeedAvailable = function() {
	return this.seedBuf.available();
};

SeedGenerator.prototype.getConnectOptions = function() {
	return this.connectOptions;
};

module.exports = SeedGenerator;
