/*
 Copyright (c) 2013, Nicos Panayides <nicosp@gmail.com>
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 Reads data from all available devices and outputs them to stdout.

*/

#define __STDC_FORMAT_MACROS

#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif /* __STDC_VERSION__ */

#include <inttypes.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include "quantisusb.h"

/**
 Data for benchmark: 10Mb
*/
#define BENCHMARK_BYTES (10*1024*1024)

static volatile int should_exit;
static uint64_t total_bytes;

void onsigterm(int dummy)
{
	should_exit = 1;
}

/**
* Called when data is read from a device
*/
static void on_read(QuantisUSBDevice *device, const unsigned char *data, int data_len)
{
	write(STDOUT_FILENO, data, (size_t)data_len);
	fsync(STDOUT_FILENO);

	total_bytes += (uint64_t)data_len;
}

/**
* Called when there is an error when reading from a device
*/
static void on_error(QuantisUSBDevice *device)
{
	perror("Quantis error");
}

static void on_device(QuantisUSBDevice *device, int present)
{
}

static int should_open_device(QuantisUSBDevice *device)
{
	return 1;
}

int main(int argc, char **argv)
{
	QuantisUSBContext *ctx;
	fd_set readfds;
	fd_set writefds;
	fd_set errorfds;
	int nfds;
	int select_status;
	struct timeval timeout;
	struct timespec start;
	struct timespec end;
	int benchmark = 1;

	signal(SIGTERM, onsigterm);
        signal(SIGINT, onsigterm);

	ctx = quantis_usb_init(on_read, on_error, on_device, should_open_device, NULL, NULL);

	quantis_usb_enable_hotplug(ctx, 1);

	if (quantis_usb_device_count(ctx) < 1) {
		fprintf(stderr, "No Quantis USB devices found\n");
		quantis_usb_destroy(ctx);
		return 1;
	}

	if (benchmark) {
		if (clock_gettime(CLOCK_MONOTONIC, &start)) {
			fprintf(stderr, "Monotonic clock not available. Benchmark disabled\n");
			benchmark = 0;
		}
	}

	while(!should_exit) {
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_ZERO(&errorfds);

		timeout.tv_sec = 120;
		timeout.tv_usec = 0;
		nfds = 0;

		quantis_usb_read_all(ctx);

		select_status = quantis_usb_before_poll(ctx, &nfds, &readfds, &writefds, &errorfds, &timeout);
		if (select_status) {
			perror("Quantis error");
			break;
		}

		select_status = select(nfds, &readfds, &writefds, &errorfds, &timeout);
		if (select_status < 0 && errno != EINTR) {
			perror("select error");
			break;
		}

		select_status = quantis_usb_after_poll(ctx, !select_status, &readfds, &writefds, &errorfds);
		if (select_status < 0) {
			perror("Quantis error");
			break;
		}

		if (benchmark && total_bytes > BENCHMARK_BYTES) {
			break;
		}

	}

	if (benchmark) {
		if (clock_gettime(CLOCK_MONOTONIC, &end)) {
			fprintf(stderr, "Monotonic clock not available. Benchmark disabled\n");
		} else {
			time_t millis;

			millis = ((end.tv_sec - start.tv_sec) * 1000) + ((end.tv_nsec - start.tv_nsec) / 1000000);

			double seconds = (double)millis / 1000.0;

			fprintf(stderr, "Total read: %" PRIu64 ". Read rate: %.0f bytes/sec\n", total_bytes, (double)total_bytes / seconds);
		}
	}

	quantis_usb_destroy(ctx);
	return 0;
}
