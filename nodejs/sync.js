#!/usr/bin/env node
/*
 Example application using the seedgenerator sync API
 and two seedgenerators for redundancy.

 Runs ROUNDS_PER_BURST "rounds" every BURST_INTERVAL.
 Stops at TOTAL_ROUNDS rounds.

*/
var seedgen = require('./seedgenerator.js');
var fs = require('fs');
var program = require('commander');

var TOTAL_ROUNDS = 1000000;
var ROUNDS_PER_MS = 8;

var DEFAULT_PORT = 4545;
var DEFAULT_HOST = 'localhost';
var DEFAULT_BUFFER_SIZE = 1024*1024;

var totalRounds = 0;
var started = false;
var startTime = 0;

function default_param(param, def) {
    return typeof param !== 'undefined' ? param : def;
}

program
  .version('1.0')
  .option('-b, --buffer <size>', 'Random bytes buffer size. (Default: ' + DEFAULT_BUFFER_SIZE + ')', parseInt)
  .option('-h, --host <host>', 'Network RNGD host. (Default: ' + DEFAULT_HOST + ')')
  .option('-o, --output <file>', 'Write all seeds received to this file.')
  .option('-p, --port <port>', 'Network RNGD port. (Default: ' + DEFAULT_PORT + ')', parseInt)
  .parse(process.argv);

if (program.output) {
    var writeStream = fs.createWriteStream(program.output, {'flags': 'a'});
    process.on('exit', function() { writeStream.end(); });
}

process.on('exit', function() {
    console.log('Total Rounds: ' + totalRounds + ' Rate: ' + totalRounds / (process.uptime() - startTime)  + ' rounds/s');
});

process.on('SIGINT', function() {
    process.exit(1);
});

var generator = new seedgen(connectOptions={host: default_param(program.host, DEFAULT_HOST), port: default_param(program.port, DEFAULT_PORT)},
	size=default_param(program.buffer, DEFAULT_BUFFER_SIZE), autoconnect=true);

generator.on('connect', function () { 
    console.log('remote seed generator ' + generator.getConnectOptions().host + ' connected');
});

generator.on('disconnect', function(message) {
    console.log('remote seed generator ' + generator.getConnectOptions().host + ' disconnected ' + message);
});

generator.on('entropy-received', function(seed) {
    //console.log('received: ' + seed.length + ' bytes of entropy from ' + generator.getConnectOptions().host + '. Entropy available: ' + getSeedAvailable());
});

function playRound() {
      var seed = new Buffer(64);
      var i;

      for (i=0; i < ROUNDS_PER_MS; i++) {
          generator.getSeed(seed);

	  //console.log('Seed of size ' + seed.length + ' bytes received from seed generator ' + generator.getConnectOptions().host + '. Available: ' + generator.getSeedAvailable());

          if (typeof writeStream !== 'undefined') {
              writeStream.write(seed);
          }

          totalRounds++;

	  if (totalRounds >= TOTAL_ROUNDS) {
              process.exit(code=0);
          } 
      }

      setTimeout(playRound, 1);
}

function playAll() {
	/* Start when at least one generator is ready */
	if (started === true) {
		return;
	}
	started = true;
	startTime = process.uptime();

	global.setImmediate(playRound);
}

generator.on('ready', playAll);
generator.connect();
